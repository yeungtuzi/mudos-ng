/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 *
 * Unit tests for HeartbeatThread / HeartbeatThreadPool.
 * Tests: lifecycle, task posting, concurrent stress, object sharding,
 *        deadlock detection.
 */
#include "base/internal/heartbeat_thread.h"
#include "base/package_api.h"
#include <event2/event.h>
#include <event2/thread.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "vm/internal/eval_limit.h"

class HBTestEnv : public ::testing::Environment { public: void SetUp() override { evthread_use_pthreads(); } };
static auto g_hb_env = ::testing::AddGlobalTestEnvironment(new HBTestEnv);

#define WAIT_FOR(cond, ms) do { auto dl=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms); while(!(cond)&&std::chrono::steady_clock::now()<dl) std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while(0)

// Minimal stubs
static object_t*mkobj(){auto*o=(object_t*)calloc(1,sizeof(object_t));o->flags.store(0x01,std::memory_order_relaxed);return o;}
static void rmobj(object_t*o){free(o);}
static program_t*mkprog(){auto*p=(program_t*)calloc(1,sizeof(program_t));p->heart_beat=0;p->ref.store(1,std::memory_order_relaxed);return p;}
static void rmprog(program_t*p){free(p);}

TEST(HeartbeatThreadTest,CreateStartStop){HeartbeatThread t(0);t.start();EXPECT_NE(t.base(),nullptr);t.stop();}
TEST(HeartbeatThreadTest,MultipleCycles){HeartbeatThread t(0);for(int i=0;i<3;i++){t.start();std::atomic<bool>done{false};t.post([&](){done=true;});WAIT_FOR(done.load(),1000);EXPECT_TRUE(done.load());t.stop();}}
TEST(HeartbeatThreadTest,IdAssignment){HeartbeatThread t0(0),t1(1),t99(99);EXPECT_EQ(t0.id(),0);EXPECT_EQ(t1.id(),1);EXPECT_EQ(t99.id(),99);}
TEST(HeartbeatThreadTest,PostSingleTask){HeartbeatThread t(0);t.start();std::atomic<bool>done{false};t.post([&](){done=true;});WAIT_FOR(done.load(),1000);EXPECT_TRUE(done.load());t.stop();}
TEST(HeartbeatThreadTest,TaskRunsOnWorker){HeartbeatThread t(0);t.start();auto cid=std::this_thread::get_id();std::atomic<std::thread::id> tid{cid};t.post([&](){tid=std::this_thread::get_id();});WAIT_FOR(tid.load()!=cid,1000);EXPECT_NE(tid.load(),cid);t.stop();}
TEST(HeartbeatThreadTest,TaskFifo){HeartbeatThread t(0);t.start();std::vector<int> ord;std::mutex m;for(int i=0;i<20;i++)t.post([i,&ord,&m](){std::lock_guard lk(m);ord.push_back(i);});std::atomic<bool>done{false};t.post([&](){done=true;});WAIT_FOR(done.load(),2000);ASSERT_EQ(ord.size(),20u);for(int i=0;i<20;i++)EXPECT_EQ(ord[i],i);t.stop();}
TEST(HeartbeatThreadTest,ConcurrentPost){constexpr int NP=8,NT=500;HeartbeatThread t(0);t.start();std::atomic<int>ctr{0};std::vector<std::thread> pts;for(int p=0;p<NP;p++)pts.emplace_back([&](){for(int i=0;i<NT;i++)t.post([&](){ctr.fetch_add(1);});});for(auto&th:pts)th.join();WAIT_FOR(ctr.load()==NP*NT,5000);EXPECT_EQ(ctr.load(),NP*NT);t.stop();}
TEST(HeartbeatThreadTest,NoDeadlockConcurrentPostStop){HeartbeatThread t(0);t.start();std::atomic<bool>kp{true};std::atomic<int>pc{0};std::thread pt([&](){while(kp.load()){t.post([&](){pc.fetch_add(1);});std::this_thread::sleep_for(std::chrono::microseconds(100));}});std::this_thread::sleep_for(std::chrono::milliseconds(200));kp=false;pt.join();auto t0=std::chrono::steady_clock::now();t.stop();auto el=std::chrono::steady_clock::now()-t0;EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(el).count(),5000);}
TEST(HeartbeatThreadTest,NoDeadlockDoubleStop){HeartbeatThread t(0);t.start();std::atomic<bool>done{false};t.post([&](){done=true;});WAIT_FOR(done.load(),1000);t.stop();t.stop();}
TEST(HeartbeatThreadTest,StressManyTasks){constexpr int NC=8,BS=200;HeartbeatThreadPool pool(4);pool.start();std::atomic<int>tot{0};std::atomic<bool>run{true};std::vector<std::thread> clrs;for(int c=0;c<NC;c++)clrs.emplace_back([&](){for(int i=0;i<BS&&run.load();i++){auto*ob=mkobj();auto*th=pool.thread_for_object(ob);th->post([&tot,ob](){tot.fetch_add(1);rmobj(ob);});if(i%50==0)std::this_thread::sleep_for(std::chrono::microseconds(50));}});for(auto&t:clrs)t.join();run=false;WAIT_FOR(tot.load()>=NC*BS,10000);EXPECT_GE(tot.load(),NC*BS);pool.stop();}

TEST(HeartbeatThreadPoolTest,CreateAndDestroy){HeartbeatThreadPool pool(2);pool.start();EXPECT_EQ(pool.size(),2u);pool.stop();}
TEST(HeartbeatThreadPoolTest,ShardingDeterministic){HeartbeatThreadPool pool(4);pool.start();auto*ob=mkobj();auto*t1=pool.thread_for_object(ob);auto*t2=pool.thread_for_object(ob);EXPECT_EQ(t1,t2);rmobj(ob);pool.stop();}
TEST(HeartbeatThreadPoolTest,ShardingValid){HeartbeatThreadPool pool(4);pool.start();for(int i=0;i<100;i++){auto*ob=mkobj();auto*t=pool.thread_for_object(ob);ASSERT_NE(t,nullptr);ASSERT_GE(t->id(),0);ASSERT_LT(t->id(),4);rmobj(ob);}pool.stop();}
TEST(HeartbeatThreadPoolTest,PostToShardedThread){HeartbeatThreadPool pool(2);pool.start();auto*ob=mkobj();auto*th=pool.thread_for_object(ob);std::atomic<bool>done{false};th->post([&](){done=true;});WAIT_FOR(done.load(),1000);EXPECT_TRUE(done.load());rmobj(ob);pool.stop();}
