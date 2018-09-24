/*
 Copyright (c) 2018 AIS, Inc.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "../rpc-broker.h"


static int create_rule(struct rule *current, char *rule)
{
    if (!rule || rule[0] == '\0')
        return -1;

    current->rule_string = strdup(rule);

    char *ruleptr;
    const char *delimiter = " ";
    char *token = strtok_r(rule, delimiter, &ruleptr);

    if (!token)
        return -1;

    current->policy = token[0] == 'a' ? 1 : 0;
    token = strtok_r(NULL, delimiter, &ruleptr);

    while (token) {

        char *field = strtok_r(NULL, delimiter, &ruleptr);

        if (!field && token[0] != 's')
            return -1;

        switch (token[0]) {
            case ('d'): {
                if (token[1] == 'e')
                    current->destination = strdup(field);
                else
                    current->domtype = strdup(field);
                break;
            }

            case ('i'): {
                if (token[1] == 'n')
                    current->interface = strdup(field);
                else {
                    current->if_bool = strdup(field);
                    token = strtok_r(NULL, delimiter, &ruleptr);
                    current->if_bool_flag = token[0] == 't' ? 1 : 0;
                }
                break;
            }

            case ('s'):
                current->stubdom = 1;
                break;

            case ('p'):
                current->path = strdup(field);
                break;

            case ('m'):
                current->member = strdup(field);
                break;

			case ('a'):
                // "out any"
				break;

            default:
                DBUS_BROKER_WARNING("Unrecognized Rule-Token: %s", token);
                current = NULL;
                break;
        }

        token = strtok_r(NULL, delimiter, &ruleptr);
    }

    return 0;
}

static inline void get_rules(DBusConnection *conn, struct domain_policy *dom)
{
    for (int rule_idx=0; rule_idx < MAX_RULES; rule_idx++) {
        char *arg;
        DBUS_REQ_ARG(arg, "/vm/%s/rpc-firewall-rules/%d",
                     dom->uuid, rule_idx);

        char *rulestring = db_query(conn, arg);

        free(arg);

        if (!rulestring)
            break;

        struct rule *policy_rule = &(dom->rules[rule_idx]);

        if (create_rule(policy_rule, rulestring) < 0)
            free_rule(*policy_rule);
        else
            dom->count++;

        free(rulestring);
    }

}

// XXX split up 
static inline void get_etc_policy(struct etc_policy *etc,
                                  const char *rule_filepath)
{
    struct stat policy_stat;
    etc->count = 0;

    if (stat(rule_filepath, &policy_stat) < 0) {
        DBUS_BROKER_WARNING("/etc policy stat of file <%s> failed %s",
                             rule_filepath, strerror(errno));
        return;
    }

    char *filename = basename(rule_filepath);

    if (filename[0] == '\0') {
        DBUS_BROKER_WARNING("/etc policy invalid file <%s>", rule_filepath);
        return;
    }

    etc->filename = filename;

    size_t policy_size = policy_stat.st_size;

    if (policy_size > ETC_MAX_FILE) {
        DBUS_BROKER_WARNING("/etc policy file %s exceeds buffer size <%d>",
                             rule_filepath, policy_size);
        return;
    }

    etc->filepath = rule_filepath;

    int policy_fd = open(rule_filepath, O_RDONLY);

    if (policy_fd < 0) {
        DBUS_BROKER_WARNING("/etc policy file %s failed to open %s",
                              rule_filepath, strerror(errno));
        return;
    }

    int rbytes = read(policy_fd, etc->etc_file, policy_size);

    if (rbytes < 0) {
        close(policy_fd);
        DBUS_BROKER_WARNING("/etc policy file %s invalid read %s",
                              rule_filepath, strerror(errno));
        return;
    }

    etc->etc_file[rbytes] = '\0';

    const char *newline = "\n";
    char *fileptr;
    char *rule_token = strtok_r(etc->etc_file, newline, &fileptr);
    int idx = 0;

    while (rule_token &&  idx < MAX_RULES) {
        if (isalpha(rule_token[0])) {
            char *line = strdup(rule_token);
            struct rule *current = &(etc->rules[idx]);
            if (create_rule(current, line) < 0)
                free_rule(*current);
            else
                idx++;
            free(line);
        }

        rule_token = strtok_r(NULL, newline, &fileptr);
    }

    etc->count = idx;
}

DBusMessage *db_list(void)
{
    DBusConnection *conn = create_dbus_connection();

    if (!conn)
        return NULL;

    struct dbus_message dmsg;

    dbus_default(&dmsg);
    dmsg.member = DBUS_LIST;
    dmsg.args[0] = (void *) DBUS_VM_PATH;

    DBusMessage *vms = make_dbus_call(conn, &dmsg);

    if (dbus_message_get_type(vms) == DBUS_MESSAGE_TYPE_ERROR) 
        vms = NULL;

    return vms;
}

struct policy *build_policy(const char *rule_filename)
{
    DBusMessage *vms = db_list();
    DBusConnection *conn = create_dbus_connection();

    struct policy *dbus_policy = calloc(1, sizeof *dbus_policy);

    DBusMessageIter iter, sub;
    dbus_message_iter_init(vms, &iter);
    dbus_message_iter_recurse(&iter, &sub);

    for (int dom_idx=0;
             dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID;
             dom_idx++) {

        void *arg;
        dbus_message_iter_get_basic(&sub, &arg);

        struct domain_policy *current = &(dbus_policy->domains[dom_idx]);
        strcpy(arg, current->uuid);
        current->domid = strtol(arg + DOMID_SECTION, NULL, 10);

        get_rules(conn, current);

        dbus_message_iter_next(&sub);
        dbus_policy->domain_number++;
    }

    struct etc_policy *etc = &(dbus_policy->etc);
    get_etc_policy(etc, rule_filename);

    dbus_message_unref(vms);

    return dbus_policy;
}

void free_rule(struct rule r)
{
    if (r.destination)
        free(r.destination);

    if (r.path)
        free(r.path);

    if (r.interface)
        free(r.interface);

    if (r.member)
        free(r.member);

    if (r.if_bool)
        free(r.if_bool);

    if (r.domtype)
        free(r.domtype);

    if (r.rule_string)
        free(r.rule_string);
}

void free_policy(void)
{
    int count = dbus_broker_policy->domain_number;

    for (int i=0; i < count; i++) {

        struct domain_policy domain = dbus_broker_policy->domains[i];
        for (int i=0; i < domain.count; i++)
            free_rule(domain.rules[i]);
    }

    struct etc_policy etc = dbus_broker_policy->etc; 
    count = etc.count;

    for (int i=0; i < count; i++) 
        free_rule(etc.rules[i]);

    free(dbus_broker_policy);
}

