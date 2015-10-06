/*
 * Copyright 2015 Artem Savkov <artem.savkov@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <bitlbee/bitlbee.h>
#include <bitlbee/http_client.h>
#include <bitlbee/json.h>
#include <bitlbee/json_util.h>

#define DISCORD_URL "http://discordapp.com/api"
#define DISCORD_HOST "discordapp.com"

typedef struct _discord_data {
  char   *token;
  char   *id;
  GSList *servers;
} discord_data;

typedef struct _server_info {
  char                 *name;
  char                 *id;
  GSList               *users;
  GSList               *channels;
  struct im_connection *ic;
} server_info;

typedef struct _cdata {
  struct im_connection *ic;
  struct groupchat *gc;
} cdata;

static void discord_http_get(struct im_connection *ic, const char *api_path,
                             http_input_function cb_func, gpointer data);

static void free_server_info(server_info *sinfo) {
  g_free(sinfo->name);
  g_free(sinfo->id);

  g_slist_free(sinfo->users);
  g_slist_free_full(sinfo->channels, (GDestroyNotify)imcb_chat_free);

  g_free(sinfo);
}

static void discord_logout(struct im_connection *ic) {
  discord_data *dd = ic->proto_data;

  g_print("%s\n", __func__);
  g_free(dd->token);
  g_free(dd->id);

  g_slist_free_full(dd->servers, (GDestroyNotify)free_server_info);

  g_free(dd);
}

static void discord_dump_http_reply(struct http_request *req) {
  g_print("============================\nstatus=%d\n", req->status_code);
  g_print("\nrh=%s\nrb=%s\n", req->reply_headers, req->reply_body);
}

static void discord_chat_msg(struct groupchat *gc, json_value *minfo) {
  g_print("<%s> %s\n", json_o_str(json_o_get(minfo, "author"), "username"),
                       json_o_str(minfo, "content"));
  imcb_chat_msg(gc, json_o_str(json_o_get(minfo, "author"), "username"),
                (char *)json_o_str(minfo, "content"), 0, 0);
}

static void discord_messages_cb(struct http_request *req) {
  cdata *cd = req->data;
  struct im_connection *ic = cd->ic;
  struct groupchat *gc = cd->gc;
  discord_dump_http_reply(req);

  if (req->status_code == 200) {
    int i;
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_array) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }

    for (i = js->u.array.length - 1; i >= 0; i--) {
      if(js->u.array.values[i]->type == json_object) {
        json_value *minfo = js->u.array.values[i];
        discord_chat_msg(gc, minfo);
      }
    }
    json_value_free(js);
    imcb_connected(ic);
    g_free(cd);
  } else {
    imcb_error(ic, "Failed to get channel info.");
    imc_logout(ic, TRUE);
    g_free(cd);
  }
}

static void discord_add_channel(server_info *sinfo, json_value *cinfo) {
  struct im_connection *ic = sinfo->ic;
  g_print("cname=%s; cid=%s; ctype=%s; clmid=%s\n",
          json_o_str(cinfo, "name"),
          json_o_str(cinfo, "id"),
          json_o_str(cinfo, "type"),
          json_o_str(cinfo, "last_message_id"));

  if (g_strcmp0(json_o_str(cinfo, "type"), "text") == 0) {
    char *topic;
    GSList *l;
    // FIXME: save it to proto data, cleanup on logout

    topic = g_strdup_printf("%s/%s", sinfo->name, json_o_str(cinfo, "name"));
    struct groupchat *gc = imcb_chat_new(ic, json_o_str(cinfo, "name"));
    imcb_chat_name_hint(gc, json_o_str(cinfo, "name"));
    g_free(topic);

    for (l = sinfo->users; l; l = l->next) {
      bee_user_t *bu = l->data;
      if (bu->ic == ic) {
        imcb_chat_add_buddy(gc, bu->handle);
      }
    }

    imcb_chat_add_buddy(gc, ic->acc->user);
    sinfo->channels = g_slist_prepend(sinfo->channels, gc);

    GString *api_path = g_string_new("");
    // TODO: limit should be configurable
    g_string_printf(api_path, "channels/%s/messages?limit=20", json_o_str(cinfo, "id"));
    cdata *cd = g_new0(cdata, 1);
    cd->ic = sinfo->ic;
    cd->gc = gc;
    discord_http_get(ic, api_path->str, discord_messages_cb, cd);
    g_string_free(api_path, TRUE);
  }
}

static void discord_channels_cb(struct http_request *req) {
  server_info *sinfo = req->data;
  struct im_connection *ic = sinfo->ic;
  discord_dump_http_reply(req);

  if (req->status_code == 200) {
    int i;
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_array) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }

    for (i = 0; i < js->u.array.length; i++) {
      if(js->u.array.values[i]->type == json_object) {
        json_value *cinfo = js->u.array.values[i];
        discord_add_channel(sinfo, cinfo);
      }
    }
    json_value_free(js);
    imcb_connected(ic);
  } else {
    imcb_error(ic, "Failed to get channel info.");
    imc_logout(ic, TRUE);
  }
}

static void discord_users_cb(struct http_request *req) {
  server_info *sinfo = req->data;
  struct im_connection *ic = sinfo->ic;
  discord_dump_http_reply(req);

  if (req->status_code == 200) {
    int i;
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_array) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }

    for (i = 0; i < js->u.array.length; i++) {
      if(js->u.array.values[i]->type == json_object) {
        json_value *uinfo = json_o_get(js->u.array.values[i], "user");
        const char *name = json_o_str(uinfo, "username");

        g_print("uname=%s\n", name);
        if (name && !bee_user_by_handle(ic->bee, ic, name)) {
          bee_user_t *buser;
          imcb_add_buddy(ic, name, NULL);
          // TODO: nickhint?
          buser = bee_user_by_handle(ic->bee, ic, name);
          sinfo->users = g_slist_prepend(sinfo->users, buser);
        }

      }
    }

    json_value_free(js);

    GString *api_path = g_string_new("");
    g_string_printf(api_path, "guilds/%s/channels", sinfo->id);
    discord_http_get(ic, api_path->str, discord_channels_cb, sinfo);
    g_string_free(api_path, TRUE);
  } else {
    imcb_error(ic, "Failed to get user list.");
    imc_logout(ic, TRUE);
  }
}

static void discord_servers_cb(struct http_request *req) {
  struct im_connection *ic = req->data;
  discord_dump_http_reply(req);

  if (req->status_code == 200) {
    int i;
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_array) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }

    for (i = 0; i < js->u.array.length; i++) {
      if(js->u.array.values[i]->type == json_object) {
        discord_data *dd = ic->proto_data;
        server_info *sinfo = g_new0(server_info, 1);
        GString *api_path = g_string_new("");
        json_value *ginfo = js->u.array.values[i];
        g_print("gname=%s; gid=%s\n", json_o_str(ginfo, "name"),
                                      json_o_str(ginfo, "id"));

        sinfo->name = json_o_strdup(ginfo, "name");
        sinfo->id = json_o_strdup(ginfo, "id");
        sinfo->ic = ic;

        g_string_printf(api_path, "guilds/%s/members", json_o_str(ginfo, "id"));
        discord_http_get(ic, api_path->str, discord_users_cb, sinfo);

        dd->servers = g_slist_prepend(dd->servers, sinfo);
        g_string_free(api_path, TRUE);
      }
    }
    json_value_free(js);
  } else {
    imcb_error(ic, "Failed to get server info.");
    imc_logout(ic, TRUE);
  }
}

static void discord_me_cb(struct http_request *req) {
  struct im_connection *ic = req->data;
  discord_dump_http_reply(req);

  if (req->status_code == 200) {
    json_value *js = json_parse(req->reply_body, req->body_size);
    if (!js || js->type != json_object) {
      imcb_error(ic, "Failed to parse json reply.");
      imc_logout(ic, TRUE);
      json_value_free(js);
      return;
    }
    discord_data *dd = ic->proto_data;
    GString *api_path = g_string_new("");

    dd->id = json_o_strdup(js, "id");
    g_print("ID: %s\n", dd->id);

    g_string_printf(api_path, "users/%s/guilds", dd->id);
    discord_http_get(ic, api_path->str, discord_servers_cb, ic);

    g_string_free(api_path, TRUE);
    json_value_free(js);
  } else {
    imcb_error(ic, "Failed to get info about self.");
    imc_logout(ic, TRUE);
  }
}

static void discord_login_cb(struct http_request *req) {
  struct im_connection *ic = req->data;
  discord_dump_http_reply(req);

  json_value *js = json_parse(req->reply_body, req->body_size);
  if (!js || js->type != json_object) {
    imcb_error(ic, "Failed to parse json reply.");
    imc_logout(ic, TRUE);
    json_value_free(js);
    return;
  }
  if (req->status_code == 200) {
    discord_data *dd = ic->proto_data;
    dd->token = json_o_strdup(js, "token");
    g_print("TOKEN: %s\n", dd->token);

    discord_http_get(ic, "users/@me", discord_me_cb, ic);
  } else {
    JSON_O_FOREACH(js, k, v){
      if (v->type != json_array) {
        continue;
      }

      int i;
      GString *err = g_string_new("");
      g_string_printf(err, "%s:", k);
      for (i = 0; i < v->u.array.length; i++) {
        if(v->u.array.values[i]->type == json_string) {
          g_string_append_printf(err, " %s",
                                 v->u.array.values[i]->u.string.ptr);
        }
      }
      imcb_error(ic, err->str);
      g_string_free(err, TRUE);
      imc_logout(ic, FALSE);
    }
  }
  json_value_free(js);
}

static void discord_login(account_t *acc) {
  struct im_connection *ic = imcb_new(acc);
  GString *request = g_string_new("");
  GString *jlogin = g_string_new("");

  ic->proto_data = g_new0(discord_data, 1);
  g_print("%s\n", __func__);
  g_string_printf(jlogin, "{\"email\":\"%s\",\"password\":\"%s\"}",
                  acc->user,
                  acc->pass);

  g_string_printf(request, "POST /api/auth/login HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: Bitlbee-Discord\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zd\r\n\r\n"
                  "%s",
                  DISCORD_HOST,
                  jlogin->len,
                  jlogin->str);

  g_print("Sending req:\n----------\n%s\n----------\n", request->str);
  (void) http_dorequest(DISCORD_HOST, 80, 0, request->str, discord_login_cb,
                       acc->ic);

  g_string_free(jlogin, TRUE);
  g_string_free(request, TRUE);
}

G_MODULE_EXPORT void init_plugin(void)
{
  struct prpl *dpp;

  static const struct prpl pp = {
    .name = "discord",
    .login = discord_login,
    .logout = discord_logout,
    /*.init = discord_init,
    .buddy_msg = fb_buddy_msg,
    .send_typing = fb_send_typing,
    .add_buddy = fb_add_buddy,
    .remove_buddy = fb_remove_buddy,
    .chat_invite = fb_chat_invite,
    .chat_leave = fb_chat_leave,
    .chat_msg = fb_chat_msg,
    .chat_join = fb_chat_join,
    .chat_topic = fb_chat_topic,*/
    .handle_cmp = g_strcmp0
  };
  g_print("%s\n", __func__);
  dpp = g_memdup(&pp, sizeof pp);
  register_protocol(dpp);
}

static void discord_http_get(struct im_connection *ic, const char *api_path,
                             http_input_function cb_func, gpointer data) {
  discord_data *dd = ic->proto_data;
  GString *request = g_string_new("");
  g_string_printf(request, "GET /api/%s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: Bitlbee-Discord\r\n"
                  "Content-Type: application/json\r\n"
                  "authorization: %s\r\n\r\n",
                  api_path,
                  DISCORD_HOST,
                  dd->token);

  g_print("Sending req:\n----------\n%s\n----------\n", request->str);
  (void) http_dorequest(DISCORD_HOST, 80, 0, request->str, cb_func,
                        data);
  g_string_free(request, TRUE);
}
