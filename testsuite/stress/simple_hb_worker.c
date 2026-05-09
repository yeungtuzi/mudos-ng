int counter;
void create() { counter = 0; set_heart_beat(1); }
void heart_beat() { counter++; mapping m = ([]); m["key"] = 1; }
int query_counter() { return counter; }
