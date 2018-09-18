/*
 * Label data operations
 * Copyright (C) 2017 Wazuh Inc.
 * February 27, 2017.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "shared.h"
#include "file_op.h"
#include "version_op.h"
#include "config/client-config.h"
#include "wazuh_modules/syscollector/syscollector.h"
#include <time.h>

/* Append a new label into an array of (size) labels at the moment of inserting. Returns the new pointer. */
wlabel_t* labels_add(wlabel_t *labels, size_t * size, const char *key, const char *value, unsigned int hidden, int overwrite) {
    size_t i;

    if (overwrite) {
        for (i = 0; labels && labels[i].key; i++) {
            if (!strcmp(labels[i].key, key)) {
                break;
            }
        }
    } else {
        i = *size;
    }

    if (!labels || i == *size) {
        os_realloc(labels, (*size + 2) * sizeof(wlabel_t), labels);
        labels[(*size)++].key = strdup(key);
        memset(labels + *size, 0, sizeof(wlabel_t));
    } else if (labels) {
        free(labels[i].value);
    }

    labels[i].value = strdup(value);
    labels[i].flags.hidden = hidden;
    return labels;
}

/* Search for a key at a label array and get the value, or NULL if no such key found. */
const char* labels_get(const wlabel_t *labels, const char *key) {
    int i;

    if (!labels) {
        return NULL;
    }

    for (i = 0; labels[i].key; i++) {
        if (!strcmp(labels[i].key, key)) {
            return labels[i].value;
        }
    }

    return NULL;
}

/* Free label array */
void labels_free(wlabel_t *labels) {
    int i;

    if (labels) {
        for (i = 0; labels[i].key != NULL; i++) {
            free(labels[i].key);
            free(labels[i].value);
        }

        free(labels);
    }
}

/* Format label array into string. Return 0 on success or -1 on error. */
int labels_format(const wlabel_t *labels, char *str, size_t size) {
    int i;
    size_t z = 0;
    char * value_label;
    for (i = 0; labels[i].key != NULL; i++) {
        value_label = parse_environment_labels(labels[i]);
        z += (size_t)snprintf(str + z, size - z, "%s\"%s\":%s\n",
            labels[i].flags.hidden ? "!" : "",
            labels[i].key,
            value_label);

        if (z >= size)
            return -1;
    }

    return 0;
}

/*
 * Parse labels from agent-info file.
 * Returns pointer to new null-key terminated array.
 * If no such file, returns NULL.
 * Free resources with labels_free().
 */
wlabel_t* labels_parse(const char *path) {
    char buffer[OS_MAXSTR];
    char *key;
    char *value;
    char *end;
    unsigned int hidden;
    size_t size = 0;
    wlabel_t *labels;
    FILE *fp;

    if (!(fp = fopen(path, "r"))) {
        if (errno == ENOENT) {
            mdebug1(FOPEN_ERROR, path, errno, strerror(errno));
        } else {
            merror(FOPEN_ERROR, path, errno, strerror(errno));
        }

        return NULL;
    }

    os_calloc(1, sizeof(wlabel_t), labels);

    /*
    "key1":value1\n
    !"key2":value2\n
    */

    while (fgets(buffer, OS_MAXSTR, fp)) {
        switch (*buffer) {
        case '!':
            if (buffer[1] == '\"') {
                hidden = 1;
                key = buffer + 2;
            } else {
                continue;
            }

            break;
        case '\"':
            hidden = 0;
            key = buffer + 1;
            break;
        default:
            continue;
        }

        if (!(value = strstr(key, "\":"))) {
            continue;
        }

        *value = '\0';
        value += 2;

        if (!(end = strchr(value, '\n'))) {
            continue;
        }

        *end = '\0';
        labels = labels_add(labels, &size, key, value, hidden, 0);
    }

    fclose(fp);
    return labels;
}

// Duplicate label array
wlabel_t * labels_dup(const wlabel_t * labels) {
    wlabel_t * copy = NULL;
    int i;

    if (!labels) {
        return NULL;
    }

    os_malloc(sizeof(wlabel_t), copy);

    for (i = 0; labels[i].key; i++) {
        os_realloc(copy, sizeof(wlabel_t) * (i + 2), copy);
        os_strdup(labels[i].key, copy[i].key);
        os_strdup(labels[i].value, copy[i].value);
    }

    copy[i].key = copy[i].value = NULL;
    return copy;
}

#define OS_COMMENT_MAX 1024
//

char * parse_environment_labels(const wlabel_t label) {

    static char final[OS_COMMENT_MAX + 1] = { '\0' };
    char orig[OS_COMMENT_MAX + 1] = { '\0' };
    char *field;
    char *str;
    char *var;
    char *end;
    char *tok;
    size_t n = 0;
    size_t z;
    cJSON *os_info;
    cJSON *network_info;
    cJSON *iface = cJSON_CreateArray();
    cJSON *ipv4 = cJSON_CreateArray();
    cJSON *ipv6 = cJSON_CreateArray();
    int i;
    char *ipv4_address;
    char *ipv6_address;
    char * mac;
    char * timeinfo;
    char * timezone;
    strncpy(orig, label.value, OS_COMMENT_MAX);

    for (str = orig; (tok = strstr(str, "$(")); str = end) {
        field = NULL;
        *tok = '\0';
        var = tok + 2;

        if (n + (z = strlen(str)) >= OS_COMMENT_MAX)
            return strdup(str);
        strncpy(&final[n], str, z);
        n += z;

        if (!(end = strchr(var, ')'))) {
            *tok = '$';
            str = tok;
            break;
        }

        *(end++) = '\0';

        // Find static fields
        if (!strcmp(var, "os.name")) {
            os_info = getunameJSON();
            field = cJSON_Print(cJSON_GetObjectItem(os_info,"os_name"));

        } else if (!strcmp(var, "os.version")) {
            os_info = getunameJSON();
            field = cJSON_Print(cJSON_GetObjectItem(os_info,"os_version"));

        } else if(!strcmp(var,"ipv4.primary")){
            network_info = getNetworkIfaces_linux();
              iface = cJSON_GetArrayItem(network_info,default_network_iface);
              ipv4 = cJSON_GetObjectItem(iface,"ipv4");
              ipv4_address = cJSON_Print(cJSON_GetObjectItem(ipv4,"address"));
            field = ipv4_address;
        } else if(!strcmp(var,"ipv6.primary")){
            network_info = getNetworkIfaces_linux();
              iface = cJSON_GetArrayItem(network_info,default_network_iface);
              ipv6 = cJSON_GetObjectItem(iface,"ipv6");
              ipv6_address = cJSON_Print(cJSON_GetObjectItem(ipv6,"address"));
            field = ipv6_address;
        }else if(!strcmp(var,"ipv4.others")){
          network_info = getNetworkIfaces_linux();
          for(i = 0; i < cJSON_GetArraySize(network_info);i++){
            if(i!=default_network_iface){
              iface = cJSON_GetArrayItem(network_info,i);
              ipv4 = cJSON_GetObjectItem(iface,"ipv4");
              ipv4_address = cJSON_Print(cJSON_GetObjectItem(ipv4,"address"));
              if(field){
                strcat(field,",");
                strcat(field,ipv4_address);
              }
              else
                field = ipv4_address;
            }
          }
        }else if(!strcmp(var,"ipv6.others")){
          network_info = getNetworkIfaces_linux();
          for(i = 0; i < cJSON_GetArraySize(network_info);i++){
            if(i!=default_network_iface){
              iface = cJSON_GetArrayItem(network_info,i);
              ipv6 = cJSON_GetObjectItem(iface,"ipv6");
              ipv6_address = cJSON_Print(cJSON_GetObjectItem(ipv6,"address"));
              if(field){
                strcat(field,",");
                strcat(field,ipv6_address);
              }
              else
                field = ipv6_address;
            }
          }
        }else if(!strcmp(var,"mac.primary")){
            network_info = getNetworkIfaces_linux();
            iface = cJSON_GetArrayItem(network_info,default_network_iface);
            mac = cJSON_Print(cJSON_GetObjectItem(iface,"mac"));
            field = mac;
        }else if(!strcmp(var,"mac.others")){
          network_info = getNetworkIfaces_linux();
          for(i = 0; i < cJSON_GetArraySize(network_info);i++){
            if(i!=default_network_iface){
              iface = cJSON_GetArrayItem(network_info,i);
              mac = cJSON_Print(cJSON_GetObjectItem(iface,"mac"));
              if(field){
                strcat(field,",");
                strcat(field,mac);
              }
              else
                field = mac;
            }
          }
        }else  if(!strcmp(var,"timezone")){
          int zone;
            time_t t = time(NULL);
            struct  tm lt={0};
            char timezone_number[21];
            localtime_r(&t, &lt);
            zone = lt.tm_gmtoff/3600;
            sprintf(timezone_number,"%d",zone);
            field = timezone_number;
        }else if(!strcmp(var,"hostname")){
          os_info = getunameJSON();
          field = cJSON_Print(cJSON_GetObjectItem(os_info,"hostname"));
        }


        if (field) {
            if (n + (z = strlen(field)) >= OS_COMMENT_MAX){

                return strdup(field);
            }
        }
        else{

            field = var;
        }

            strncpy(&final[n], field, z);
            n += z;

    }

    if (n + (z = strlen(str)) >= OS_COMMENT_MAX){


        return strdup(field);
    }

    strncpy(&final[n], str, z);
    final[n + z] = '\0';
    return strdup(final);

}
