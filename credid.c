#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "credid.h"

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }
  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

#define credid_api_obfuscate_password(query)            \
  ({                                                    \
    int len = strlen(query);                            \
    int test = 1;                                       \
    for (int i = 4; i < len; i++) {                     \
      if (query[i] == ':')                              \
        break;                                          \
      if (query[i] != ' ')                              \
        test = 0;                                       \
    }                                                   \
    if (strncmp("AUTH", query, 4) == 0 && test == 1) {  \
      int position = 0;                                 \
      for (int i = 4; i < len; i++) {                   \
        if (position == 0) {                            \
          if (query[i] == ':')                          \
            position = 1;                               \
        }                                               \
        else if (position == 1) {                       \
          if (query[i] != ' ')                          \
            position = 2;                               \
        }                                               \
        else if (position == 2) {                       \
          if (query[i] == ' ') {                        \
            position = 3;                               \
            query[i] = '\n';                            \
          }                                             \
        }                                               \
        else if (position == 3) {                       \
          query[i] = 0;                                 \
        }                                               \
      }                                                 \
      printf("Safe query = %s\n", query);               \
    }                                                   \
  })

// TODO: check malloc
#define credid_api_log(api, _query, _status)                            \
  ({                                                                    \
    if (api->logs_enabled == 1) {                                       \
      credid_api_log_t *new_log = (credid_api_log_t*)malloc(sizeof(credid_api_log_t)); \
      credid_api_logs_link_t *new_link = (credid_api_logs_link_t*)malloc(sizeof(credid_api_logs_link_t)); \
      new_log->query = strdup(_query);                                  \
      credid_api_obfuscate_password(new_log->query);                    \
      new_log->status = _status;                                        \
      new_link->line = new_log;                                         \
      new_link->next = NULL;                                            \
      if (api->logs_end != NULL)                                        \
        api->logs_end->next = new_link;                                 \
      api->logs_end = new_link;                                         \
      if (api->logs == NULL)                                            \
        api->logs = new_link;                                           \
    }                                                                   \
  })

// TODO: check recv, snprintf
#define credid_api_send(api, options, format, ...)                      \
  ({                                                                    \
    int err = 0;                                                        \
    char cmd[MAXDATASIZE] = {0};                                        \
    char cmd_tmp[MAXDATASIZE] = {0};                                    \
    snprintf(cmd, MAXDATASIZE-1, format "\n" , ##__VA_ARGS__);          \
    va_list varglist;                                                   \
    va_start(varglist, options);                                        \
    for (int i = 0; i < options; i++) {                                 \
      char *option = va_arg(varglist, char*);                           \
      snprintf(cmd_tmp, MAXDATASIZE-1, "%s %s", option, cmd);           \
      strncpy(cmd, cmd_tmp, MAXDATASIZE);                               \
    }                                                                   \
    va_end(varglist);                                                   \
    send(api->socket, cmd, strlen(cmd), 0);                             \
    int numbytes;                                                       \
    if (api->last_command_result != NULL)                               \
      free(api->last_command_result);                                   \
    api->last_command_result = (char *)malloc(MAXDATASIZE);             \
    if (api->last_command_result != NULL) {                             \
      memset(api->last_command_result, 0, MAXDATASIZE);                 \
      if ((numbytes = recv(api->socket, api->last_command_result, MAXDATASIZE-1, 0)) == -1) { \
        err = 0xFF;                                                     \
      }                                                                 \
    }                                                                   \
    else {                                                              \
      err = 0xFE;                                                       \
    }                                                                   \
    credid_api_log(api, cmd, err || credid_api_success(api) == 1 ? 0 : 1); \
    err;                                                                \
  })


credid_api_t *credid_api_init(char const *host, short unsigned int port) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;
  int rv;
  char s[INET6_ADDRSTRLEN];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char port_string[6] = {0};
  snprintf(port_string, 5, "%hui", port);
  printf("Connect to: %s:%s\n", host, port_string);
  if ((rv = getaddrinfo(host, port_string, &hints, &servinfo)) != 0) {
    perror("getaddrinfo");
    return NULL;
  }

  // loop through all the results and connect to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      perror("socket");
      continue;
    }

    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("connect");
      continue;
    }

    break;
  }

  if (p == NULL) {
    fprintf(stderr, "client: failed to connect\n");
    return NULL;
  }

  inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
  printf("client: connecting to %s\n", s);

  freeaddrinfo(servinfo); // all done with this structure

  credid_api_t *api = (credid_api_t *)malloc(sizeof(credid_api_t));
  if (api == NULL){
    close(sockfd);
    return NULL;
  }
  api->socket = sockfd;
  api->last_command_result = NULL;
  api->logs_enabled = 0;
  api->logs = NULL;
  api->logs_end = NULL;
  return api;
}

int credid_api_free(credid_api_t *api) {
  close(api->socket);
  if (api->last_command_result != NULL)
    free(api->last_command_result);
  credid_api_free_logs(api);
  free(api);
  return 0;
}

char *credid_api_last_result(credid_api_t const *api) {
  return api->last_command_result + 8;
}

int credid_api_success(credid_api_t const *api) {
  return strncmp(api->last_command_result, "success", 7) == 0 ? 1 : 0;
}

int _credid_api_auth(credid_api_t *api, char const *username, char const *password, int options, ...) {
  return credid_api_send(api, options, "AUTH : %s %s", username, password);
}

int _credid_api_user_has_access_to(credid_api_t *api, char const *username, char const *perm, char const *resource, int options, ...) {
  return credid_api_send(api, options, "USER HAS ACCESS TO : %s %s %s", username, perm, resource);
}

int _credid_api_group_add(credid_api_t *api, char const *group, char const *perm, char const *resource, int options, ...) {
  return credid_api_send(api, options, "GROUP ADD : %s %s %s", group, perm, resource);
}

int _credid_api_group_remove(credid_api_t *api, char const *group, char const *resource, int options, ...) {
  return credid_api_send(api, options, "GROUP REMOVE : %s %s", group, resource);
}

int _credid_api_group_list(credid_api_t *api, int options, ...) {
  return credid_api_send(api, options, "GROUP LIST");
}

int _credid_api_group_list_perms(credid_api_t *api, char const *group, int options, ...) {
  return credid_api_send(api, options, "GROUP LIST PERMS : %s", group);
}

int _credid_api_group_get_perm(credid_api_t *api, char const *group, char const *resource, int options, ...) {
  return credid_api_send(api, options, "GROUP GET PERM : %s %s", group, resource);
}

int _credid_api_user_list(credid_api_t *api, int options, ...) {
  return credid_api_send(api, options, "USER LIST");
}

int _credid_api_user_add(credid_api_t *api, char const *username, char const *password, int options, ...) {
  return credid_api_send(api, options, "USER ADD : %s %s", username, password);
}

int _credid_api_user_remove(credid_api_t *api, char const *username, int options, ...) {
  return credid_api_send(api, options, "USER REMOVE : %s", username);
}

int _credid_api_user_add_group(credid_api_t *api, char const *username, char const *group, int options, ...) {
  return credid_api_send(api, options, "USER ADD GROUP : %s %s", username, group);
}

int _credid_api_user_remove_group(credid_api_t *api, char const *username, char const *group, int options, ...) {
  return credid_api_send(api, options, "USER REMOVE GROUP : %s %s", username, group);
}

int _credid_api_user_list_groups(credid_api_t *api, char const *username, int options, ...) {
  return credid_api_send(api, options, "USER LIST GROUPS : %s", username);
}

int _credid_api_user_change_password(credid_api_t *api, char const *username, char const *newpassword, int options, ...) {
  return credid_api_send(api, options, "USER CHANGE PASSWORD : %s %s", username, newpassword);
}

int credid_api_setup_logs(credid_api_t *api, int enable) {
  api->logs_enabled = enable;
  return 0;
}

credid_api_log_t *credid_api_fetch_log(credid_api_t *api) {
  credid_api_logs_link_t *current_logs_link = api->logs;
  if (current_logs_link == NULL)
    return NULL;
  else {
    credid_api_log_t *current_log = current_logs_link->line;
    api->logs = current_logs_link->next;
    free(current_logs_link);
    // if it was the last log, set the end at NULL
    if (api->logs == NULL)
      api->logs_end = NULL;
    return current_log;
  }
}

int credid_api_free_logs(credid_api_t *api) {
  credid_api_logs_link_t *current = api->logs, *next = NULL;
  while (current != NULL) {
    next = current->next;
    free(current->line->query);
    free(current->line);
    free(current);
    current = next;
  }
  api->logs = NULL;
  api->logs_end = NULL;
  return 0;
}
