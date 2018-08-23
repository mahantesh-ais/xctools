#include "../rpc-broker.h"


DBusConnection *create_dbus_connection(void)
{
    DBusError error;
    dbus_error_init(&error);

    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

    if (dbus_error_is_set(&error)) 
        DBUS_BROKER_WARNING("<DBus Connection Error> [%s]", error.message);

    return conn;
}

struct dbus_broker_server *start_server(int port)
{
    struct dbus_broker_server *server = malloc(sizeof *server);

    server->dbus_socket = v4v_socket(SOCK_STREAM);
    if (server->dbus_socket < 0)
        DBUS_BROKER_ERROR("v4v_socket");

	server->addr.port = port;
	server->addr.domain = V4V_DOMID_ANY;

    if (v4v_bind(server->dbus_socket, &server->addr, V4V_DOMID_ANY) < 0)
        DBUS_BROKER_ERROR("v4v_bind");

    if (v4v_listen(server->dbus_socket, 1) < 0)
        DBUS_BROKER_ERROR("v4v_listen");

    return server;
}

void dbus_default(struct dbus_message *dmsg)
{
    dmsg->dest = DBUS_DB_DEST;
    dmsg->iface = DBUS_DB_IFACE;
    dmsg->path = DBUS_BASE_PATH;
    dmsg->arg_number = 1;
    snprintf(dmsg->arg_sig, 2, "s");
}

int connect_to_system_bus(void)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0)
        DBUS_BROKER_ERROR("socket");

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    // could have bus-path override (LEN would need checking)
    snprintf(addr.sun_path, DBUS_BUS_ADDR_LEN, DBUS_BUS_ADDR);

    if (connect(srv, (struct sockaddr *) &addr, sizeof(addr)) < 0) 
        DBUS_BROKER_ERROR("connect");

    return srv;
}

static inline void append_variant(DBusMessageIter *iter, int type, void *data)
{
    DBusMessageIter sub;

    char *dbus_sig;
    int dbus_type;

    switch (type) {

        case ('i'):
            dbus_sig = "i";
            dbus_type = DBUS_TYPE_INT32;
            break;

        case ('u'):
            dbus_sig = "u";
            dbus_type = DBUS_TYPE_UINT32;
            break;

        case ('b'):
            dbus_sig = "b";
            dbus_type = DBUS_TYPE_BOOLEAN;
            break;

        case ('s'):
            dbus_sig = "s";
            dbus_type = DBUS_TYPE_STRING;
            break;

        case ('d'):
            dbus_sig = "d";
            dbus_type = DBUS_TYPE_DOUBLE;
            break;

        default:
            DBUS_BROKER_WARNING("Unrecognized DBus Variant <%d>", type);
            break;
    }

    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, dbus_sig, &sub);

    if (type == 's')
        dbus_message_iter_append_basic(&sub, dbus_type, &data);
    else
        dbus_message_iter_append_basic(&sub, dbus_type, data);

    dbus_message_iter_close_container(iter, &sub); 
}

DBusMessage *make_dbus_call(DBusConnection *conn, struct dbus_message *dmsg)
{
    DBusMessage *msg = dbus_message_new_method_call(dmsg->dest, dmsg->path,
                                                    dmsg->iface, dmsg->method);
    DBusError error;
    dbus_error_init(&error);

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);

    for (int i=0; i < dmsg->arg_number; i++) {

        switch (dmsg->arg_sig[i]) {

            case ('s'):
                dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, 
                                               &(dmsg->args[i])); 
                break;

            case ('u'):
                dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32,
                                                dmsg->args[i]);
                break;

            case ('i'): 
                dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, 
                                                dmsg->args[i]);
                break;

            case ('b'): {
                dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, 
                                               dmsg->args[i]); 
                break;
            }

            case ('v'): {
                append_variant(&iter, dmsg->json_sig[i], dmsg->args[i]);
                break;
            }

            default:
                DBUS_BROKER_WARNING("<Invalid DBus Signature> [%c]",
                                      dmsg->arg_sig[i]); 
                break;
        }
    }

    DBusPendingCall *pc = NULL;
    if (!dbus_connection_send_with_reply(conn, msg, 
                                        &pc, DBUS_REQ_TIMEOUT) || !pc) 
        return NULL;

    dbus_connection_flush(conn);
    dbus_pending_call_block(pc);
    dbus_connection_flush(conn);

    dbus_message_unref(msg);

    if ((msg = dbus_pending_call_steal_reply(pc)) == NULL) 
        return NULL;

    dbus_connection_unref(conn);

    return msg;
}

char *db_query(DBusConnection *conn, char *arg)
{
    char *reply = malloc(sizeof(char) * RULE_MAX_LENGTH);

    struct dbus_message *dmsg = calloc(sizeof *dmsg + sizeof(char *), 1);
    dbus_default(dmsg);
    dmsg->method = DBUS_READ;
    dmsg->args[0] = (void *) arg;

    DBusMessage *msg = make_dbus_call(conn, dmsg);
    free(dmsg);

    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) 
        return NULL;

    dbus_message_iter_get_basic(&iter, &reply);
    dbus_message_unref(msg);

    if (reply[0] == '\0') 
        return NULL;

    return reply;
}

char *dbus_introspect(struct json_request *jreq)
{
    struct dbus_message *dmsg = calloc(1, sizeof *dmsg);

    dmsg->dest = jreq->dmsg->dest;
    dmsg->iface = DBUS_INTRO_IFACE;
    dmsg->method = DBUS_INTRO_METH;
    dmsg->path = jreq->dmsg->path;
    dmsg->arg_number = 0;
    dmsg->arg_sig[0] = '\0';

    dbus_connection_flush(jreq->conn);
    DBusMessage *introspect = make_dbus_call(jreq->conn, dmsg);

    if (dbus_message_get_type(introspect) == DBUS_MESSAGE_TYPE_ERROR)
        return NULL;

    char *reply = malloc(sizeof(char) * 4096);

    DBusMessageIter iter;
    dbus_message_iter_init(introspect, &iter);
    dbus_message_iter_get_basic(&iter, &reply);

    char *signature = calloc(1, sizeof(char) * 16);
    if (retrieve_xml_signature(reply, signature, jreq->dmsg->iface, 
                                                 jreq->dmsg->method) < 1)
        signature[0] = '\0';

    return signature;
}
