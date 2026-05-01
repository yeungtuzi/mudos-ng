// Various mud extension protocol using telnet.

#include "base/package_api.h"
#include "base/internal/io_thread.h"

#include "thirdparty/libtelnet/libtelnet.h"  // FIXME?

/* TELNET */
#ifdef F_REQUEST_TERM_TYPE
void f_request_term_type() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    telnet_request_ttype(ip->telnet);
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message(
        "Warning: wrong usage. request_term_type() should only be called by a user object.\n");
  }
}
#endif

#ifdef F_START_REQUEST_TERM_TYPE
void f_start_request_term_type() {
  auto *ip = command_giver->interactive;
  if (ip && ip->telnet) {
    telnet_start_request_ttype(ip->telnet);
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message(
        "Warning: wrong usage. start_request_term_type() should only be called by a user "
        "object.\n");
  }
}
#endif

#ifdef F_REQUEST_TERM_SIZE
void f_request_term_size() {
  auto *ip = current_object->interactive;

  if (ip && ip->telnet) {
    if ((st_num_arg == 1) && (sp->u.number == 0)) {
      telnet_dont_naws(ip->telnet);
    } else {
      telnet_do_naws(ip->telnet);
    }
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message(
        "Warning: wrong usage. request_term_size() should only be called by a user object.\n");
  }
  if (st_num_arg == 1) {
    sp--;
  }
}
#endif

#ifdef F_TELNET_NOP
void f_telnet_nop() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    telnet_send_nop(ip->telnet);
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. telnet_nop() should only be called by a user object.\n");
  }
}
#endif

#ifdef F_TELNET_GA
void f_telnet_ga() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    telnet_send_ga(ip->telnet);
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. telnet_ga() should only be called by a user object.\n");
  }
}
#endif

/* MXP */
#ifdef F_HAS_MXP
void f_has_mxp() {
  int i = 0;

  if (sp->u.ob->interactive) {
    i = sp->u.ob->interactive->iflags & USING_MXP;
    i = !!i;  // force 1 or 0
  }
  free_object(&sp->u.ob, "f_has_mxp");
  put_number(i);
}
#endif

#ifdef F_ACT_MXP
void f_act_mxp() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    // start MXP
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { telnet_begin_sb(ip->telnet, TELNET_TELOPT_MXP); });
    } else {
      telnet_begin_sb(ip->telnet, TELNET_TELOPT_MXP);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. act_mxp() should only be called by a user object.\n");
  }
}
#endif

/* GMCP */

#ifdef F_HAS_GMCP
void f_has_gmcp() {
  int i = 0;

  if (sp->u.ob->interactive) {
    i = sp->u.ob->interactive->iflags & USING_GMCP;
    i = !!i;  // force 1 or 0
  }
  free_object(&sp->u.ob, "f_has_gmcp");
  put_number(i);
}
#endif

#ifdef F_SEND_GMCP
void f_send_gmcp() {
  auto *ip = current_object->interactive;
  const auto *data = sp->u.string;
  auto len = SVALUE_STRLEN(sp);
  if (ip && ip->telnet) {
    std::string const transdata = u8_convert_encoding(ip->trans, data, len);
    std::string_view const result = transdata.empty() ? std::string_view(data, len) : std::string_view(transdata);

    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      std::string copy(result.data(), result.size());
      ip->io_thread->post([ip, copy = std::move(copy)]() {
        telnet_subnegotiation(ip->telnet, TELNET_TELOPT_GMCP, copy.data(), copy.size());
        flush_message(ip);
      });
    } else {
      telnet_subnegotiation(ip->telnet, TELNET_TELOPT_GMCP, result.data(), result.size());
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. send_gmcp() should only be called by a user object.\n");
  }
  pop_stack();
}
#endif

/* MSP */

#ifdef F_HAS_MSP
void f_has_msp() {
  int i = 0;

  if (sp->u.ob->interactive) {
    i = sp->u.ob->interactive->iflags & USING_MSP;
    i = !!i;  // force 1 or 0
  }
  free_object(&sp->u.ob, "f_has_msp");
  put_number(i);
}
#endif

/* MSDP */

#ifdef F_HAS_MSDP
void f_has_msdp() {
  int i = 0;

  if (sp->u.ob->interactive) {
    i = sp->u.ob->interactive->iflags & USING_MSDP;
    i = !!i;  // force 1 or 0
  }
  free_object(&sp->u.ob, "f_has_msdp");
  put_number(i);
}
#endif

#ifdef F_SEND_MSDP_VARIABLE
/** TODO update to support sending other value type and use proper MSDP mapping/array formats as needed based on data types */
void f_send_msdp_variable() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      // Extract data from LPC stack on VM thread, post telnet ops to IO thread
      // to serialize access to telnet->z (MCCP compression stream).
      std::string var_name((sp - 1)->u.string);
      int type = sp->type;
      std::string str_val;
      long num_val = 0;
      double real_val = 0.0;

      switch (type) {
        case T_STRING:
          str_val.assign(sp->u.string, SVALUE_STRLEN(sp));
          break;
        case T_NUMBER:
          num_val = sp->u.number;
          break;
        case T_REAL:
          real_val = sp->u.real;
          break;
        case T_BUFFER:
          str_val.assign(reinterpret_cast<char *>(sp->u.buf->item), sp->u.buf->size);
          break;
        case T_ARRAY:
        case T_MAPPING:
        default:
          error("Bad argument 2 to send_msdp_variable()\n");
          break;
      }

      ip->io_thread->post([ip, type, var_name = std::move(var_name),
                           str_val = std::move(str_val), num_val, real_val]() {
        telnet_begin_sb(ip->telnet, TELNET_TELOPT_MSDP);
        switch (type) {
          case T_STRING:
            telnet_printf(ip->telnet, "\x01%s\x02", var_name.c_str());
            telnet_send(ip->telnet, str_val.data(), str_val.size());
            break;
          case T_NUMBER:
            telnet_printf(ip->telnet, "\x01%s\x02%lu", var_name.c_str(), num_val);
            break;
          case T_REAL:
            telnet_printf(ip->telnet, "\x01%s\x02%f", var_name.c_str(), real_val);
            break;
          case T_BUFFER:
            telnet_printf(ip->telnet, "\x01%s\x02", var_name.c_str());
            telnet_send(ip->telnet, str_val.data(), str_val.size());
            break;
        }
        telnet_finish_sb(ip->telnet);
        flush_message(ip);
      });
    } else {
      switch(sp->type) {
        case T_STRING:
          telnet_begin_sb(ip->telnet, TELNET_TELOPT_MSDP);
          telnet_printf(ip->telnet, "\x01%s\x02", (sp - 1)->u.string);
          telnet_send(ip->telnet, sp->u.string, SVALUE_STRLEN(sp));
          telnet_finish_sb((ip->telnet));
          break;
        case T_NUMBER:
          telnet_begin_sb(ip->telnet, TELNET_TELOPT_MSDP);
          telnet_printf(ip->telnet, "\x01%s\x02%lu", (sp - 1)->u.string,  sp->u.number);
          telnet_finish_sb((ip->telnet));
          break;
        case T_REAL:
          telnet_begin_sb(ip->telnet, TELNET_TELOPT_MSDP);
          telnet_printf(ip->telnet, "\x01%s\x02%f", (sp - 1)->u.string,  sp->u.real);
          telnet_finish_sb((ip->telnet));
          break;
        case T_BUFFER:
          telnet_begin_sb(ip->telnet, TELNET_TELOPT_MSDP);
          telnet_printf(ip->telnet, "\x01%s\x02", (sp - 1)->u.string);
          telnet_send(ip->telnet, reinterpret_cast<char *>(sp->u.buf->item), sp->u.buf->size);
          telnet_finish_sb((ip->telnet));
          break;
        case T_ARRAY:
        case T_MAPPING:
        default:
          error("Bad argument 2 to send_msdp_variable()\n");
          break;
      }
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. send_msdp_variable() should only be called by a user object.\n");
  }
  pop_2_elems();
}
#endif

/* ZMP */

#ifdef F_HAS_ZMP
void f_has_zmp() {
  int i = 0;

  if (sp->u.ob->interactive) {
    i = sp->u.ob->interactive->iflags & USING_ZMP;
    i = !!i;  // force 1 or 0
  }
  free_object(&sp->u.ob, "f_has_zmp");
  put_number(i);
}
#endif

#ifdef F_SEND_ZMP
void f_send_zmp() {
  auto *ip = command_giver->interactive;
  if (ip && ip->telnet) {
    // Extract data on VM thread before posting to IO thread
    std::string cmd((sp - 1)->u.string);
    auto *arr = sp->u.arr;
    std::vector<std::string> args;
    for (int i = 0; i < arr->size; i++) {
      if (arr->item[i].type == T_STRING) {
        args.emplace_back(arr->item[i].u.string);
      }
    }

    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip, cmd = std::move(cmd), args = std::move(args)]() {
        telnet_begin_zmp(ip->telnet, cmd.c_str());
        for (const auto &arg : args) {
          telnet_zmp_arg(ip->telnet, arg.c_str());
        }
        telnet_finish_zmp(ip->telnet);
        flush_message(ip);
      });
    } else {
      telnet_begin_zmp(ip->telnet, cmd.c_str());
      for (const auto &arg : args) {
        telnet_zmp_arg(ip->telnet, arg.c_str());
      }
      telnet_finish_zmp(ip->telnet);
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message("Warning: wrong usage. send_zmp() should only be called by a user object.\n");
  }
  pop_2_elems();
}
#endif

#ifdef F_TELNET_MSP_OOB
void f_telnet_msp_oob() {
  auto *ip = current_object->interactive;
  if (ip && ip->telnet) {
    telnet_send_msp_oob(ip, sp->u.string, SVALUE_STRLEN(sp));
    // flush_message only works on the IO thread; post if needed.
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
      ip->io_thread->post([ip]() { flush_message(ip); });
    } else {
      flush_message(ip);
    }
  } else if (!ip) {
    debug_message(
        "Warning: wrong usage. telnet_msp_oob() should only be called by a user object.\n");
  }
  pop_stack();
}
#endif
