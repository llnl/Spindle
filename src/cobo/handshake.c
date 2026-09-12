/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <netinet/ip.h>

#include "handshake.h"
#include "config.h"

#if defined(MUNGE)
#include <munge.h>
#endif

#if defined(GCRYPT)
#include <gcrypt.h>
#endif

#if !defined MUNGE_TTL_TIMEOUT_SEC
#define MUNGE_TTL_TIMEOUT_SEC 30
#endif
#define MAX_NUM_TIMEOUTS 3

#if !defined(MUNGE) && !defined(GCRYPT) && !defined(ENABLE_NULL_ENCRYPTION)
#error No communication types defined.  Handshake will not work
#endif

#define BASE_FILE ((strrchr(__FILE__, '/') ? : __FILE__ - 1) + 1)

#define debug_printf(format, ...)                                       \
   do {                                                                 \
      if (debug_file) {                                                 \
         fprintf(debug_file, "[%d@%s:%u] - " format, getpid(), BASE_FILE, __LINE__, ## __VA_ARGS__); \
      }                                                                 \
   } while (0)

#define error_printf(format, ...)                                       \
   do {                                                                 \
      log_error(format, ## __VA_ARGS__);                                \
      if (debug_file) {                                                 \
         fprintf(debug_file, "ERROR: [%d@%s:%u] - %s", getpid(),  BASE_FILE, __LINE__, last_error_message); \
      }                                                                 \
   } while (0)

#define security_error_printf(format, ...)                              \
   do {                                                                 \
       log_security_error(format, ## __VA_ARGS__);                      \
       if (debug_file) {                                                \
          fprintf(debug_file, "SECURITY ERROR: [%d%s:%u] - %s", getpid(), BASE_FILE, __LINE__, last_security_message); \
       }                                                                \
   } while (0)

#define MAX_ADDR_LEN 14
#define SIG 0x845D96C1
#define SERVER_TO_CLIENT_SIG 0x67AD047E
#define CLIENT_TO_SERVER_SIG 0x9B1CC028

#define HSHAKE_AGAIN -16
#define HOSTNAME_MAX_LEN 256

typedef struct {
   uint64_t random_number;
   uint64_t session_id;
   uint32_t counter;
   char hostname[HOSTNAME_MAX_LEN];
} random_number_t;

typedef struct {
   random_number_t random_number;
   uint32_t signature;
   uint16_t server_port;
   uint16_t client_port;
   uid_t uid;
   gid_t gid;
   unsigned char server_addr[MAX_ADDR_LEN];
   unsigned char client_addr[MAX_ADDR_LEN];
} handshake_packet_t;

typedef struct {
   random_number_t random_number;
   uint32_t signature;
} random_number_packet_t;

typedef struct {
   int i_am_server;
   struct sockaddr server_addr;
   struct sockaddr client_addr;
} connection_info_t;

static FILE *debug_file = NULL;
static char *last_error_message = NULL;
static char *last_security_message = NULL;
static unsigned char *saved_key;
static char *saved_key_filepath;
static unsigned int saved_key_len;
static connection_info_t *saved_conninfo;
static int timeout_seconds = 0;
static uint32_t unique_number_counter = 0;

/** Routines for creating a handshake_packet_t **/
static int encode_addr(struct sockaddr *addr, unsigned char *target_addr, uint16_t *port);
static int encode_packet(handshake_packet_t *packet, uint64_t session_id,
                         struct sockaddr *server_addr, struct sockaddr *client_addr);

/** Routines for turning a handshake_packet_t into an encrypted buffer **/
static int encrypt_packet(handshake_protocol_t *hdata, void *packet, size_t packet_size,
                          unsigned char **packet_buffer, size_t *packet_buffer_size);
static int none_encrypt_packet(void *packet, size_t packet_size,
                               unsigned char **packet_buffer, size_t *packet_buffer_size);
static int munge_encrypt_packet(void *packet, size_t packet_size,
                                unsigned char **packet_buffer, size_t *packet_buffer_size);
static int filekey_encrypt_packet(char *key_filepath, ssize_t key_length_bytes,
                                  void *packet, size_t packet_size,
                                  unsigned char **packet_buffer, size_t *packet_buffer_size);
static int key_encrypt_packet(unsigned char *key, ssize_t key_length_bytes,
                              void *packet, size_t packet_size,
                              unsigned char **packet_buffer, size_t *packet_buffer_size);

/** Routines for decrypting and validating a handshake_packet_t **/
static int decrypt_packet(handshake_protocol_t *hdata, void *recvd_packet, size_t packet_size,
                          unsigned char *recvd_buffer, size_t recvd_buffer_size);
static int none_decrypt_packet(void *recvd_packet, size_t packet_size,
                               unsigned char *recvd_buffer, size_t recvd_buffer_size);
static int munge_decrypt_packet(void *recvd_packet, size_t packet_size,
                                unsigned char *recvd_buffer, size_t recvd_buffer_size);
static int key_decrypt_packet(unsigned char *key, unsigned int key_len,
                              void *recvd_packet, size_t packet_size,
                              unsigned char *recvd_buffer, size_t recvd_buffer_size);
static int compare_packets(handshake_packet_t *expected_packet, handshake_packet_t *recvd_packet);
static int compare_random_number_packets(random_number_packet_t *expected_random_number_packet, random_number_packet_t *recvd_random_number_packet);


static int handshake_wrapper(int sockfd, handshake_protocol_t *hdata, uint64_t session_id,
                             int is_server);
static int handshake_main(int sockfd, handshake_protocol_t *hdata, uint64_t session_id,
                          int is_server);
static ssize_t reliable_write(int fd, const void *buf, ssize_t size);
static ssize_t reliable_read(int fd, void *buf, ssize_t size);
static int read_key(char *key_filepath, ssize_t key_length_bytes);
static int share_result(int fd, int result);
static int get_client_server_addrs(int sockfd, int i_am_server, connection_info_t *conninfo);
static int send_packet(int sockfd, unsigned char *packet, unsigned int packet_size);
static int recv_packet(int sockfd, unsigned char **packet, size_t *packet_size);
static int exchange_sig(int sockfd);
static int log_security_error(const char *format, ...);
static int log_error(const char *format, ...);

int spindle_handshake_server(int sockfd, handshake_protocol_t *hdata, uint64_t session_id)
{
   debug_printf("Starting handshake from server\n");
   return handshake_wrapper(sockfd, hdata, session_id, 1);
}

int spindle_handshake_client(int sockfd, handshake_protocol_t *hdata, uint64_t session_id)
{
   debug_printf("Starting handshake from client\n");
   return handshake_wrapper(sockfd, hdata, session_id, 0);
}

int spindle_handshake_is_security_type_enabled(handshake_security_t sectype)
{
   switch (sectype) {
      case hs_none:
#if defined(ENABLE_NULL_ENCRYPTION)
         return 1;
#else
         return 0;
#endif
      case hs_munge:
#if defined(MUNGE)
         return 1;
#else
         return 0;
#endif
      case hs_key_in_file:
      case hs_explicit_key:
#if defined(GCRYPT)
         return 1;
#else
         return 0;
#endif
   }
   return 0;
}

char *spindle_handshake_last_error_str()
{
   if (last_security_message)
      return last_security_message;
   else
      return last_error_message;
}

void spindle_handshake_enable_debug_prints(FILE *debug_output)
{
   debug_file = debug_output;
}

void spindle_handshake_enable_read_timeout(int timeout_sec)
{
   timeout_seconds = timeout_sec;
}

static int handshake_wrapper(int sockfd, handshake_protocol_t *hdata, uint64_t session_id,
                             int is_server)
{
   connection_info_t connection_info;
   int num_timeouts = 0, result, return_result;
   sighandler_t old_pipe_action;

   if (last_security_message)
      free(last_security_message);
   if (last_error_message)
      free(last_error_message);
   last_security_message = last_error_message = NULL;

   old_pipe_action = signal(SIGPIPE, SIG_IGN);

   /**
    * Record connection info
    **/
   saved_conninfo = &connection_info;
   result = get_client_server_addrs(sockfd, is_server, &connection_info);
   if (result < 0) {
      debug_printf("Could not get socket addresses in get_client_server_addrs. Other side must have closed.\n");
      return_result = HSHAKE_DROP_CONNECTION;
      goto done;
   }
   
   for (;;) {
      result = handshake_main(sockfd, hdata, session_id, is_server);
      if (result != HSHAKE_AGAIN) {
         /* Typical case */
         break;
      }
      /* We hit a timeout (perhaps a munge cert beyond its TTL).  Try again if num_timeouts < MAX_NUM_TIMEOUTS */
      if (++num_timeouts == MAX_NUM_TIMEOUTS) {
         security_error_printf("Peer could not produce a non-timed out certificate in %d attempts\n",
                             num_timeouts);
         result = HSHAKE_ABORT;
         break;
      }
   }
   return_result = result;

  done:
   debug_printf("Completed server handshake.  Result = %d\n", result);

   saved_conninfo = NULL;
   signal(SIGPIPE, old_pipe_action);

   return return_result;
}

static int handshake_main(int sockfd, handshake_protocol_t *hdata, uint64_t session_id,
                          int is_server)
{
   int result, return_result, peer_result, socket_error = 0;
   handshake_packet_t packet, expected_packet, recvd_packet;
   random_number_packet_t random_number_packet, expected_random_number_packet, recvd_random_number_packet;
   unsigned char *packet_buffer = NULL, *recvd_long_packet_buffer = NULL, *recvd_packet_buffer = NULL;
   size_t packet_buffer_size = 0, recvd_long_packet_buffer_size = 0, recvd_packet_buffer_size = 0;

   /**
    * Exchange a public signature as a handshake to make sure
    * we're speaking the same protocol.
    **/
   result = exchange_sig(sockfd);
   if (result < 0) {
      debug_printf("Error exchanging signatures\n");
      socket_error = 1;
      return_result = result;
      goto done;
   }

   /**
    * Encode socket names, session, random number, gid, and uid into a handshake_packet_t
    **/

   debug_printf("Creating outgoing packet for handshake\n");
   result = encode_packet(&packet, session_id, &saved_conninfo->server_addr, &saved_conninfo->client_addr);
   if (result < 0) {
      debug_printf("Error encoding outgoing packet");
      return_result = result;
      goto done;
   }
   packet.signature = is_server ? SERVER_TO_CLIENT_SIG : CLIENT_TO_SERVER_SIG;
   debug_printf("Encoded packet: server_port = %d, client_port = %d, "
                "uid = %d, gid = %d, session_id = %llu, signature = %lx, random_number =%lx\n",
                (int) packet.server_port, (int) packet.client_port, (int) packet.uid, (int) packet.gid,
                (unsigned long long) packet.random_number.session_id, (unsigned long) packet.signature, (unsigned long) packet.random_number.random_number);

   /**
    * Encrypt/Sign the handshake_packet_t, producing a packet_buffer
    **/
   debug_printf("Encrypting outgoing packet\n");
   result = encrypt_packet(hdata, &packet, sizeof(handshake_packet_t), &packet_buffer, &packet_buffer_size);
   if (result < 0) {
      debug_printf("Error in server encrypting outgoing packet");
      return_result = result;
      goto done;
   }
   debug_printf("Encrypted packet to buffer of size %lu\n", (unsigned long) packet_buffer_size);

   /**
    * Send the packet_buffer on the network
    **/
   result = send_packet(sockfd, packet_buffer, packet_buffer_size);
   free(packet_buffer);
   packet_buffer = NULL;
   if (result < 0) {
      debug_printf("Problem sending packet on network: %s\n", strerror(errno));
      return_result = result;
      socket_error = 1;
      goto done;
   }

   /**
    * Recieve a packet_buffer on the network
    **/
   result = recv_packet(sockfd, &recvd_long_packet_buffer, &recvd_long_packet_buffer_size);
   if (result < 0) {
      debug_printf("Problem receiving packet\n");
      return_result = result;
      socket_error = 1;
      goto done;
   }

   /**
    * Decrypt their initial handshake packet, containing their random number
   **/
   debug_printf("Decrypting their initial handshake packet, containing their random number\n");
   result = decrypt_packet(hdata, &recvd_packet, sizeof(handshake_packet_t), recvd_long_packet_buffer, recvd_long_packet_buffer_size);
   if (result < 0) {
      debug_printf("Error decrypting and checking received packet\n");
      return_result = result;
      goto done;
   }

   /**
    * Produce an expected handshake_packet_t, random_number_packet_t
    **/
   debug_printf("Creating an expected packet\n");
   result = encode_packet(&expected_packet, session_id, &saved_conninfo->server_addr, &saved_conninfo->client_addr);
   if (result < 0) {
      debug_printf("Error creating expected packet\n");
      return_result = result;
      goto done;
   }

   expected_packet.signature  = is_server ? CLIENT_TO_SERVER_SIG : SERVER_TO_CLIENT_SIG;
   
   debug_printf("Checking random number packets\n");

   debug_printf("Checking initial packet\n");
   result = compare_packets( &expected_packet, &recvd_packet);
   if (result < 0) {
      debug_printf("Error checking initial packet\n");
      return_result = result;
      goto done;
   }

   //encrypt random number packet that they are expecting
   random_number_packet.random_number = recvd_packet.random_number;
   random_number_packet.signature = packet.signature;
   result = encrypt_packet(hdata, &random_number_packet, sizeof(random_number_packet_t), &packet_buffer, &packet_buffer_size);
   if (result < 0) {
      debug_printf("Error in server encrypting outgoing random_number_packet");
      return_result = result;
      goto done;
   }

   //send their expected packet random number
   debug_printf("Sending their expected random number packet on network\n");
   result = send_packet(sockfd, packet_buffer, packet_buffer_size);
   free(packet_buffer);
   packet_buffer = NULL;
   if (result < 0) {
      debug_printf("Problem sending packet on network: %s\n", strerror(errno));
      return_result = result;
      socket_error = 1;
      return HSHAKE_DROP_CONNECTION;
   }

   //get thier random number packet
   debug_printf("Receiving their random number packet from network\n");
   result = recv_packet(sockfd, &recvd_packet_buffer, &recvd_packet_buffer_size);
   if (result < 0) {
      debug_printf("Problem receiving packet\n");
      return_result = result;
      socket_error = 1;
      goto done;
   }
   //decrypt their signed random number packet
   debug_printf("Decrypting their random number packet\n");
   result = decrypt_packet(hdata, &recvd_random_number_packet, sizeof(random_number_packet_t), recvd_packet_buffer, recvd_packet_buffer_size);
   if (result < 0) {
      debug_printf("Error checking initial packet\n");
      return_result = result;
      goto done;
   }

   //encrypt random number packet that they are expecting
   random_number_packet.random_number = recvd_packet.random_number;
   random_number_packet.signature = packet.signature;
   result = encrypt_packet(hdata, &random_number_packet, sizeof(random_number_packet_t), &packet_buffer, &packet_buffer_size);
   if (result < 0) {
      debug_printf("Error in server encrypting outgoing random_number_packet");
      return_result = result;
      goto done;
   }

   expected_random_number_packet.random_number = packet.random_number;
   expected_random_number_packet.signature = is_server ? CLIENT_TO_SERVER_SIG : SERVER_TO_CLIENT_SIG;

   debug_printf("Checking random number packets\n");
   result = compare_random_number_packets(&recvd_random_number_packet, &expected_random_number_packet);
   if (result < 0) {
      debug_printf("Error checking packet\n");
      return_result = result;
      goto done;
   }
   
   debug_printf("Successfully completed initial handshake\n"); 
   
   return_result = 0;

  done:

   /** 
    * Send to peer the result of our connection attempt.  Only share whether
    * we're accepting, dropping, or asking for a re-try.  
    **/
   if (!socket_error) {
      peer_result = share_result(sockfd, return_result);
      if (return_result == 0 && peer_result != 0) {
         /**
          * Only return the peer's result if we think everything
          * authenticated successfully on our end.  Otherwise we'll
          * return our result.
          **/
         debug_printf("Setting handshake result to peer's result of %d\n", peer_result);
         return_result = peer_result;
      }
   }
   if (packet_buffer)
      free(packet_buffer);
   if (recvd_packet_buffer)
      free(recvd_packet_buffer);

   return return_result;
}

static int encode_addr(struct sockaddr *addr, unsigned char *target_addr, uint16_t *port)
{
   switch (addr->sa_family) {
      case AF_INET: {
         struct sockaddr_in *addr_in = (struct sockaddr_in *) addr;
         *port = addr_in->sin_port;
         assert(sizeof(addr_in->sin_addr) < MAX_ADDR_LEN);
         memset(target_addr, 0, MAX_ADDR_LEN);
         memcpy(target_addr, &addr_in->sin_addr, sizeof(addr_in->sin_addr));
         break;
      }
      // Add other network protocols here
      default: {
         error_printf("Unsupported protocol used in sockaddr: %d\n", (int) addr->sa_family);
         return HSHAKE_INTERNAL_ERROR;
      }
   }
   return 0;
}

int random_number(random_number_t *rand_num, uint64_t session_id) {
   int local_errno;   
   int timeout = 100 * 30; //30 seconds
   useconds_t ten_milliseconds = 10000;
   ssize_t result, bytes_to_read, bytes_read;
   char hostname_buf[HOSTNAME_MAX_LEN];

    // Get hostname
   if (gethostname(hostname_buf, HOSTNAME_MAX_LEN) != 0) {
      local_errno = errno;
      error_printf("Trouble getting hostname. Error: %s\n",strerror(local_errno));
      return -1;
   }
   hostname_buf[HOSTNAME_MAX_LEN-1] = '\0';
   
   //random number file
   bytes_to_read = sizeof(rand_num->random_number);
   bytes_read = 0;
   do {
      result = getrandom(((unsigned char*) &rand_num->random_number) + bytes_read,
                         bytes_to_read - bytes_read,
                         GRND_NONBLOCK);
      if (result<0) {
         local_errno = errno;
         if (local_errno == EAGAIN || local_errno == EINTR){
            if (--timeout <= 0) {
               error_printf("Random number collection failed with repeated EAGAIN or EINTR\n");
               return -1;
            }
            usleep(ten_milliseconds);
            continue;
         }
         else{
            error_printf("Random number collection failed. Error: %s\n", strerror(local_errno));
            return -1;
         }
      }
      bytes_read += result;
   } while (bytes_read < bytes_to_read);

   rand_num->session_id = session_id;
   rand_num->counter = unique_number_counter;
   strcpy(rand_num->hostname, hostname_buf);
   unique_number_counter++;
   
   return 0;
}


static int encode_packet(handshake_packet_t *packet, uint64_t session_id,
                         struct sockaddr *server_addr, struct sockaddr *client_addr)
{   
   int result;
   packet->uid = getuid();
   packet->gid = getgid();

   result = random_number(&packet->random_number, session_id);
   if (result < 0) {
      debug_printf("Error encoding random number\n");
      return result;
   }
   
   result = encode_addr(server_addr, packet->server_addr, &packet->server_port);
   if (result < 0) {
      debug_printf("Error encoding server addr\n");
      return result;
   }
   
   result = encode_addr(client_addr, packet->client_addr, &packet->client_port);
   if (result < 0) {
      debug_printf("Error encoding client addr\n");
      return result;
   }
   
   return 0;
}

static int encrypt_packet(handshake_protocol_t *hdata, void *packet, size_t packet_size,
                          unsigned char **packet_buffer, size_t *packet_buffer_size)
{
   switch (hdata->mechanism) {
      case hs_none:
         debug_printf("Server skipping encryption of packet\n");
         return none_encrypt_packet(packet, packet_size, packet_buffer, packet_buffer_size);
      case hs_munge:
         debug_printf("Server encrypting packet with munge\n");
         return munge_encrypt_packet(packet, packet_size, packet_buffer, packet_buffer_size);
      case hs_key_in_file:
         debug_printf("Server encrypting packet with key of size %ld from file %s\n",
                      hdata->data.key_in_file.key_length_bytes,
                      hdata->data.key_in_file.key_filepath);
         return filekey_encrypt_packet(hdata->data.key_in_file.key_filepath,
                                       hdata->data.key_in_file.key_length_bytes,
                                       packet, packet_size, packet_buffer, packet_buffer_size);
      case hs_explicit_key:
         debug_printf("Server encrypting packet with provided key of size %ld\n",
                      hdata->data.explicit_key.key_length_bytes);
         return key_encrypt_packet(hdata->data.explicit_key.key,
                                   hdata->data.explicit_key.key_length_bytes,
                                   packet, packet_size, packet_buffer, packet_buffer_size);
   }
   abort();
   return HSHAKE_INTERNAL_ERROR;
}

static int none_encrypt_packet(void *packet, size_t packet_size,
                               unsigned char **packet_buffer, size_t *packet_buffer_size)
{
#if defined(ENABLE_NULL_ENCRYPTION)
   *packet_buffer_size = packet_size;
   *packet_buffer = malloc(*packet_buffer_size);
   memcpy(*packet_buffer, packet, *packet_buffer_size);
   return 0;
#else
   (void)packet;
   (void)packet_size;
   (void)packet_buffer;
   (void)packet_buffer_size;
   error_printf("Null encryption must be explicitly enabled\n");
   return HSHAKE_INTERNAL_ERROR;
#endif
}

#if defined(MUNGE)
static int munge_create_context(munge_ctx_t *output_ctx)
{
   munge_ctx_t ctx;
   munge_err_t result;

   ctx = munge_ctx_create();
   if (!ctx) {
      error_printf("Problem creating munge context\n");
      return HSHAKE_INTERNAL_ERROR;
   }
   
   result = munge_ctx_set(ctx, MUNGE_OPT_CIPHER_TYPE, MUNGE_CIPHER_AES128);
   if (result != EMUNGE_SUCCESS) {
      error_printf("Unable to set cipher type in munge: %s", munge_ctx_strerror(ctx) ? : "NO ERROR");
      return HSHAKE_INTERNAL_ERROR;
   }
   
   result = munge_ctx_set(ctx, MUNGE_OPT_TTL, MUNGE_TTL_TIMEOUT_SEC);
   if (result != EMUNGE_SUCCESS) {
      error_printf("Unable to set TTL in munge: %s", munge_ctx_strerror(ctx) ? : "NO ERROR");
      return HSHAKE_INTERNAL_ERROR;
   } 
   
   *output_ctx = ctx;
   return 0;
}
#endif

static int munge_encrypt_packet(void *packet, size_t packet_size,
                                unsigned char **packet_buffer, size_t *packet_buffer_size)
{
#if defined(MUNGE)
   munge_err_t result;
   munge_ctx_t ctx = NULL;
   int return_result;

   result = munge_create_context(&ctx);
   if (result < 0) {
      debug_printf("Failed to create munge context while encrypting packet\n");
      return_result = result;
      goto done;
   }
   
   result = munge_encode((char **) packet_buffer, ctx, packet, packet_size);
   if (result != EMUNGE_SUCCESS) {
      error_printf("Munge failed to encrypt packet with error: %s\n", munge_ctx_strerror(ctx));
      return_result = HSHAKE_INTERNAL_ERROR;
      goto done;
   }
   assert(*packet_buffer != NULL);
   *packet_buffer_size = strlen((char *) *packet_buffer) + 1;

   debug_printf("Munge encoded packet successfully\n");

   return_result = 0;

  done:
   if (ctx != NULL) {
      munge_ctx_destroy(ctx);
   }
   return return_result;
#else
   (void)packet;
   (void)packet_size;
   (void)packet_buffer;
   (void)packet_buffer_size;
   error_printf("Handshake not compiled with munge support\n");
   return HSHAKE_INTERNAL_ERROR;
#endif
}

static int read_key(char *key_filepath, ssize_t key_length_bytes)
{
   ssize_t result;
   int fd = -1;
   struct stat st;
   int return_result;
   uid_t uid = getuid();
   gid_t gid = getgid();
   unsigned char *buffer;

   if (saved_key &&
       strcmp(key_filepath, saved_key_filepath) == 0 &&
       saved_key_len == key_length_bytes)
   {
      debug_printf("Reusing saved key at %s\n", key_filepath);
      return 0;
   }
   
   if (saved_key) {
      memset(saved_key, 0, saved_key_len);
      free(saved_key);
      saved_key = NULL;
   }

   fd = open(key_filepath, O_RDONLY);
   if (fd == -1) {
      security_error_printf("Unable to open key file %s: %s\n", key_filepath, strerror(errno));
      return_result = HSHAKE_ABORT;
      goto done;
   }

   result = fstat(fd, &st);
   if (result == -1) {
      security_error_printf("Unable to access keyfile %s: %s\n", key_filepath, strerror(errno));
      return_result = HSHAKE_ABORT;
      goto done;
   }
   
   if (st.st_uid != uid) {
      security_error_printf("UID on key file %s (%d) does not match my UID (%d)\n", 
                            key_filepath, st.st_uid, uid);
      return_result = HSHAKE_ABORT;
      goto done;
   }

   if (st.st_gid != gid) {
      security_error_printf("GID on key file %s (%d) does not match my GID (%d)\n", 
                            key_filepath, st.st_gid, gid);
      return_result = HSHAKE_ABORT;
      goto done;
   }

   if ((st.st_mode & 077) != 0) {
      security_error_printf("Protections on key file %s are too loose with %o\n",
                            key_filepath, (int) st.st_mode);
      return_result = HSHAKE_ABORT;
      goto done;
   }
   
   if (st.st_size != key_length_bytes) {
      security_error_printf("Size on key file %s is %lu and does not match expected size of %lu\n",
                            key_filepath, (unsigned long) st.st_size, (unsigned long) key_length_bytes);
      return_result = HSHAKE_ABORT;
      goto done;
   }

   buffer = malloc(key_length_bytes);
   assert(buffer);
   result = reliable_read(fd, buffer, key_length_bytes);
   if (result != (ssize_t)key_length_bytes) {
      security_error_printf("Unable to read from key file %s: %s\n", key_filepath, strerror(errno));
      return_result = HSHAKE_ABORT;
      goto done;
   }

   assert(saved_key == NULL);
   saved_key = buffer;
   saved_key_len = key_length_bytes;
   saved_key_filepath = key_filepath;

   return_result = 0;

  done:
   if (fd != -1)
      close(fd);

   return return_result;
}

static int filekey_encrypt_packet(char *key_filepath, ssize_t key_length_bytes,
                                  void *packet, size_t packet_size,
                                  unsigned char **packet_buffer, size_t *packet_buffer_size)
{
   int result;

   result = read_key(key_filepath, key_length_bytes);
   if (result < 0) {
      debug_printf("Error reading key from %s\n", key_filepath);
      return result;
   }
   
   result = key_encrypt_packet(saved_key, key_length_bytes, packet, packet_size,
                               packet_buffer, packet_buffer_size);
   if (result < 0) {
      debug_printf("Error encrypting packet under filekey_encrypt\n");
      return result;
   }

   return 0;
}

#if defined(GCRYPT)
static int get_hash_of_buffer(unsigned char *buffer, size_t buffer_size,
                              unsigned char *key, int key_length_bytes,
                              unsigned char **hash_result, int *hash_result_size)
{
   gcry_md_hd_t md;
   int algo;
   gcry_error_t gcry_result;
   unsigned char *temp_result;

   debug_printf("Encrypting with GCRY_MD_SHA256\n");
   algo = GCRY_MD_SHA256;
   *hash_result_size = 32;
   
   gcry_result = gcry_md_open(&md, algo, GCRY_MD_FLAG_HMAC);
   if (gcry_result != GPG_ERR_NO_ERROR) {
      error_printf("Error initializing gcrypt: %s (%d)\n", gpg_strerror(gcry_result), (int) gcry_result);
      return HSHAKE_INTERNAL_ERROR;
   }

   gcry_result = gcry_md_setkey(md, key, key_length_bytes);
   if (gcry_result != GPG_ERR_NO_ERROR) {
      error_printf("Error initializing gcrypt: %s (%d)\n", gpg_strerror(gcry_result), (int) gcry_result);
      return HSHAKE_INTERNAL_ERROR;
   }

   gcry_md_write(md, buffer, buffer_size);

   temp_result = gcry_md_read(md, algo);
   if (temp_result == NULL) {
      error_printf("Error reading hash of message\n");
      return HSHAKE_INTERNAL_ERROR;
   }

   *hash_result = malloc(*hash_result_size);
   memcpy(*hash_result, temp_result, *hash_result_size);
   
   gcry_md_close(md);

   return 0;
}

#endif

static int key_encrypt_packet(unsigned char *key, ssize_t key_length_bytes,
                              void *packet, size_t packet_size,
                              unsigned char **packet_buffer, size_t *packet_buffer_size)
{
#if defined(GCRYPT)
   unsigned char *hash_result;
   int hash_result_size;
   int result;
   static int initialized = 0;

   if (!initialized) {
      gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
      gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
      initialized = 1;
   }
   
   result = get_hash_of_buffer((unsigned char *) packet, packet_size,
                               key, key_length_bytes,
                               &hash_result, &hash_result_size);
   if (result < 0) {
      debug_printf("Error getting has while encrypting packet\n");
      return result;
   }
   
   debug_printf("Adding packet of size %lu and hash of size %u to buffer\n",
                (unsigned long) packet_size, hash_result_size);
   *packet_buffer_size = packet_size + hash_result_size;
   *packet_buffer = malloc(*packet_buffer_size);
   memcpy(*packet_buffer, packet, packet_size);
   memcpy(*packet_buffer + packet_size, hash_result, hash_result_size);

   free(hash_result);

   return 0;   
#else
   (void)key;
   (void)key_length_bytes;
   (void)packet;
   (void)packet_size;
   (void)packet_buffer;
   (void)packet_buffer_size;
   error_printf("Handshake not built against gcrypt\n");
   return HSHAKE_INTERNAL_ERROR;
#endif   
}

static ssize_t reliable_write(int fd, const void *buf, ssize_t size)
{
   int result;
   ssize_t bytes_written = 0;
   while (bytes_written < size) {
      result = write(fd, ((unsigned char *) buf) + bytes_written, size - bytes_written);
      if (result == -1 && errno == EINTR)
         continue;
      else if (result == -1) {
         error_printf("Error writing to socket: %s\n", strerror(errno));
         return -1;
      }
      else
         bytes_written += result;
   }
   return bytes_written;
}

static ssize_t reliable_read(int fd, void *buf, ssize_t size)
{
   ssize_t result;
   ssize_t bytes_read = 0;

   while (bytes_read < size) {
      if (timeout_seconds) {
         fd_set rfd_set;
         struct timeval timeout;
         FD_ZERO(&rfd_set);
         FD_SET(fd, &rfd_set);
         timeout.tv_sec = timeout_seconds;
         timeout.tv_usec = 0;
         
         result = select(fd+1, &rfd_set, NULL, NULL, &timeout);
         if (result == -1) {
            error_printf("Error select'ing on socket fd: %s\n", strerror(errno));
            return HSHAKE_INTERNAL_ERROR;
         }
         else if (result == 0) {
            error_printf("Timed out waiting for read from peer\n");
            return HSHAKE_INTERNAL_ERROR;
         }
         else if (result != 1) {
            error_printf("Unexpected return code of %d from select\n", result);
            return HSHAKE_INTERNAL_ERROR;            
         }        
      }
      result = read(fd, ((unsigned char *) buf) + bytes_read, size - bytes_read);
      if (result <= 0) {
         int error = errno;
         if (error == ECONNRESET) {
            debug_printf("Connection reset during handshake read\n");
            return HSHAKE_DROP_CONNECTION;
         }
         error_printf("Expected error return %d when reading from socket: %s\n", result,
                      strerror(error));
         return HSHAKE_DROP_CONNECTION;
      }
      else
         bytes_read += result;
   }

   return bytes_read;
}

static int decrypt_packet(handshake_protocol_t *hdata, void *recvd_packet, size_t packet_size,
                          unsigned char *recvd_buffer, size_t recvd_buffer_size)
{
   switch (hdata->mechanism) {
      case hs_none:
         debug_printf("Checking packet with no encryption\n");
         return none_decrypt_packet(recvd_packet, packet_size, recvd_buffer, recvd_buffer_size);
      case hs_munge:
         debug_printf("Decrypting and checking packet with munge\n");
         return munge_decrypt_packet(recvd_packet, packet_size, recvd_buffer, recvd_buffer_size);
      case hs_key_in_file:
         debug_printf("Decrypting packet with key from file\n");
         assert(saved_key);
         return key_decrypt_packet(saved_key, saved_key_len,
                                   recvd_packet, packet_size, recvd_buffer, recvd_buffer_size);
      case hs_explicit_key:
         debug_printf("Decrypting packet with explicit key\n");
         return key_decrypt_packet(hdata->data.explicit_key.key, hdata->data.explicit_key.key_length_bytes,
                                   recvd_packet, packet_size, recvd_buffer, recvd_buffer_size);
   }
   abort();
   return HSHAKE_INTERNAL_ERROR;
}

static int none_decrypt_packet(void *recvd_packet, size_t packet_size,
                               unsigned char *recvd_buffer, size_t recvd_buffer_size)
{
#if defined(ENABLE_NULL_ENCRYPTION)
   if (recvd_buffer_size != packet_size) {
      error_printf("Received buffer of size %lu, but expected size %lu\n",
                   (unsigned long) recvd_buffer_size, (unsigned long) packet_size);
      return HSHAKE_DROP_CONNECTION;
   }
   memcpy(recvd_packet, recvd_buffer, recvd_buffer_size);
   return 0;
#else
   (void)recvd_packet;
   (void)packet_size;
   (void)recvd_buffer;
   (void)recvd_buffer_size;
   error_printf("Null encryption must be explicitly enabled\n");
   return HSHAKE_INTERNAL_ERROR;
#endif
}

static int munge_decrypt_packet(void *recvd_packet, size_t recvd_packet_size,
                                unsigned char *recvd_buffer, size_t recvd_buffer_size)
{
   (void)recvd_buffer_size;
#if defined(MUNGE)
   munge_err_t result;
   munge_ctx_t ctx = NULL;
   void *payload = NULL;
   int payload_size;
   int return_result = 0, iresult;
   uid_t uid;
   gid_t gid;

   iresult = munge_create_context(&ctx);
   if (iresult < 0) {
      debug_printf("Failed to create munge context while decrypting packet\n");
      return_result = iresult;
      goto done;
   }

   result = munge_decode((char *) recvd_buffer, ctx, &payload, &payload_size, &uid, &gid);
   switch (result) {
      case EMUNGE_SUCCESS:
         break;
      case EMUNGE_SNAFU:
      case EMUNGE_BAD_ARG:
      case EMUNGE_BAD_LENGTH:
      case EMUNGE_OVERFLOW:
      case EMUNGE_NO_MEMORY:
      case EMUNGE_SOCKET:
      case EMUNGE_TIMEOUT:
         error_printf("Munge failed to decrypt packet with error: %s\n", munge_strerror(result));
         return_result = HSHAKE_INTERNAL_ERROR;
         goto done;
      case EMUNGE_CRED_EXPIRED:
         debug_printf("Produced a timed out certificate.\n");
         return_result = HSHAKE_AGAIN;
         goto done;
      case EMUNGE_BAD_CRED:
         debug_printf("Received garbage credential\n");
         return_result = HSHAKE_DROP_CONNECTION;
         goto done;
      case EMUNGE_BAD_VERSION:
      case EMUNGE_BAD_CIPHER:
      case EMUNGE_BAD_MAC:
      case EMUNGE_BAD_ZIP:
      case EMUNGE_BAD_REALM:
      case EMUNGE_CRED_INVALID:
      case EMUNGE_CRED_REWOUND:
      case EMUNGE_CRED_REPLAYED:
      case EMUNGE_CRED_UNAUTHORIZED:
         security_error_printf("Bad credential provided: %s\n", munge_strerror(result));
         return_result = HSHAKE_ABORT;
         goto done;
      default:
         security_error_printf("Unknown error return from munge: %s\n", munge_strerror(result));
         return_result = HSHAKE_ABORT;
         goto done;
   }

   if (payload_size != (int)recvd_packet_size) {
      security_error_printf("Recieved munge packet with invalid payload size of %d\n", (int) payload_size);
      return_result = HSHAKE_ABORT;
      goto done;
   }
   memcpy(recvd_packet, payload, recvd_packet_size);
  done:
   if (payload)
      free(payload);
   if (ctx)
      munge_ctx_destroy(ctx);

   return return_result;
#else
   (void)recvd_packet;
   (void)recvd_packet_size;
   (void)recvd_buffer;
   (void)recvd_buffer_size;
   error_printf("Handshake not compiled with munge support\n");
   return HSHAKE_INTERNAL_ERROR;
#endif
}

static int key_decrypt_packet(unsigned char *key, unsigned int key_len,
                              void *recvd_packet, size_t packet_size,
                              unsigned char *recvd_buffer, size_t recvd_buffer_size)
{
#if defined(GCRYPT)
   unsigned char *calcd_hash_val = NULL, *recvd_hash_val;
   int result, return_result, hash_val_size;
   int i;
  
   if (recvd_buffer_size < packet_size) {
      error_printf("Packet was too small.  Size was %d, expected at least %d\n",
                   (int) recvd_buffer_size, (int) packet_size);
      return_result = HSHAKE_INTERNAL_ERROR;
      goto done;
   }
   memcpy(recvd_packet, recvd_buffer, packet_size);
   result = get_hash_of_buffer((unsigned char *) recvd_packet, packet_size,
                               key, key_len,
                               &calcd_hash_val, &hash_val_size);
   if (result < 0) {
      debug_printf("Error hashing received packet\n");
      return_result = HSHAKE_INTERNAL_ERROR;
      goto done;
   }
   
   if (recvd_buffer_size != packet_size + hash_val_size) {
      error_printf("Packet was too small.  Size was %d, expected %d\n",
                   (int) recvd_buffer_size, (int) packet_size + hash_val_size);
      return_result = HSHAKE_INTERNAL_ERROR;
      goto done;
   }

   recvd_hash_val = recvd_buffer + packet_size;
   for (i = 0; i < hash_val_size; i++) {
      if (recvd_hash_val[i] != calcd_hash_val[i]) {
         security_error_printf("Hash signature of packet did not match expected value\n");
         return_result = HSHAKE_ABORT;
         goto done;
      }
   }
   
  done:
   if (calcd_hash_val)
      free(calcd_hash_val);
   
   return return_result;
#else
   (void)key;
   (void)key_len;
   (void)recvd_packet;
   (void)packet_size;
   (void)recvd_buffer;
   (void)recvd_buffer_size;
   error_printf("handshake was not compiled with gcrypt support");
   return HSHAKE_INTERNAL_ERROR;
#endif
}

static int compare_packets(handshake_packet_t *expected_packet, handshake_packet_t *recvd_packet)
{
   int i;
   if (expected_packet->random_number.session_id != recvd_packet->random_number.session_id) {
      //If sessions don't match, expect that we've just recv a packet
      //from another instance of handshake running on the same node.
      //Drop connection
      debug_printf("Received mismatching session IDs in initial packed.  Expected %lu, got %lu. Two different spindle sessions probably tried to connect.\n",
                   expected_packet->random_number.session_id, recvd_packet->random_number.session_id);
      return HSHAKE_DROP_CONNECTION;
   }
   if (expected_packet->signature != recvd_packet->signature) {
      security_error_printf("Received initial packet with malformed signature. Packet expected %x, got %x\n",
                            expected_packet->signature, recvd_packet->signature);
      return HSHAKE_ABORT;
   }

   if (expected_packet->server_port != recvd_packet->server_port) {
      security_error_printf("Received handshake with bad server port.  Expected %d, got %d\n",
                            (int) expected_packet->server_port, (int) recvd_packet->server_port);
      return HSHAKE_ABORT;
   }

   if (expected_packet->client_port != recvd_packet->client_port) {
      security_error_printf("Received handshake with bad client port.  Expected %d, got %d\n",
                            (int) expected_packet->client_port, (int) recvd_packet->client_port);
      return HSHAKE_ABORT;
   }

   if (expected_packet->uid != recvd_packet->uid) {
      security_error_printf("Received handshake from another uid.  Expected %d, got %d\n",
                            (int) expected_packet->uid, (int) recvd_packet->uid);
      return HSHAKE_ABORT;
   }

   if (expected_packet->gid != recvd_packet->gid) {
      security_error_printf("Received handshake from another gid.  Expected %d, got %d\n",
                            (int) expected_packet->gid, (int) recvd_packet->gid);
      return HSHAKE_ABORT;
   }

   for (i = 0; i < MAX_ADDR_LEN; i++) {
      if (expected_packet->server_addr[i] != recvd_packet->server_addr[i]) {
         security_error_printf("Received handshake with incorrect server addr\n");
         return HSHAKE_ABORT;
      }
   }

   for (i = 0; i < MAX_ADDR_LEN; i++) {
      if (expected_packet->client_addr[i] != recvd_packet->client_addr[i]) {
         security_error_printf("Received handshake with incorrect client addr\n");
         return HSHAKE_ABORT;
      }
   }

   debug_printf("Packets compared equal.\n");
   return 0;
}

static int compare_random_number_packets(random_number_packet_t *expected_random_number_packet, random_number_packet_t *recvd_random_number_packet)
{
   
   if (expected_random_number_packet->signature != recvd_random_number_packet->signature) {
      security_error_printf("Received random number packet with malformed signature. Packet expected %x, got %x\n",
                           expected_random_number_packet->signature, recvd_random_number_packet->signature);
      return HSHAKE_ABORT;
   }

   const random_number_t *a = &expected_random_number_packet->random_number;
   const random_number_t *b = &recvd_random_number_packet->random_number;
   if (a->random_number != b->random_number || a->session_id != b->session_id || a->counter != b->counter || strcmp(a->hostname, b->hostname) != 0){
      security_error_printf("Received handshake from another random number.  Expected random number %ld, got %ld, session id: %lu got %lu, counter: %d got %d, hostname: %s got %s\n",
                            a->random_number, b->random_number,
                            a->session_id, b->session_id,
                            (int) a->counter, (int) b->counter,
      
                             a->hostname, b->hostname);
      return HSHAKE_ABORT;
   }

   debug_printf("Packets compared equal.\n");
   return 0;
}

static int share_result(int fd, int handshake_result)
{
   int32_t result_to_send, peer_result;
   ssize_t result;

   switch (handshake_result) {
      case HSHAKE_SUCCESS:
         result_to_send = HSHAKE_SUCCESS;
         break;
      case HSHAKE_INTERNAL_ERROR:
      case HSHAKE_DROP_CONNECTION:
      case HSHAKE_ABORT:
         result_to_send = HSHAKE_DROP_CONNECTION;
         break;
      case HSHAKE_AGAIN:
         result_to_send = HSHAKE_AGAIN;
         break;
   }
    
   debug_printf("Sharing handshake result %d with peer\n", result_to_send);
      
   result = reliable_write(fd, &result_to_send, sizeof(result_to_send));
   if (result != (ssize_t)sizeof(result_to_send)) {
      error_printf("Failed to send result of connection\n");
      return HSHAKE_INTERNAL_ERROR;
   }
   
   debug_printf("Reading peer result\n");
   result = reliable_read(fd, &peer_result, sizeof(peer_result));
   if (result != (ssize_t)sizeof(peer_result)) {
      debug_printf("Failed to read handshake result from peer\n");
      return result;
   }
   debug_printf("Peer reported result of %d\n", peer_result);

   if (peer_result == HSHAKE_SUCCESS)
      return HSHAKE_SUCCESS;
   else if (peer_result == HSHAKE_AGAIN)
      return HSHAKE_AGAIN;
   else
      return HSHAKE_CONNECTION_REFUSED;
}

static int get_client_server_addrs(int sockfd, int i_am_server,
                                   connection_info_t *conninfo)
{
   struct sockaddr remote_addr, local_addr;
   socklen_t addr_len;
   int result;

   debug_printf("Looking up server and client addresses for socket %d\n", sockfd);   
   addr_len = sizeof(remote_addr);
   result = getpeername(sockfd, &remote_addr, &addr_len);
   if (result == -1) {
      error_printf("Error getting peer socket name: %s\n", strerror(errno));
      return HSHAKE_DROP_CONNECTION;
   }
   addr_len = sizeof(remote_addr);
   result = getsockname(sockfd, &local_addr, &addr_len);
   if (result == -1) {
      error_printf("Error getting local socket name: %s\n", strerror(errno));
      return HSHAKE_DROP_CONNECTION;
   }

   conninfo->i_am_server = i_am_server;
   if (i_am_server) {
      conninfo->server_addr = local_addr;
      conninfo->client_addr = remote_addr;
   }
   else {
      conninfo->server_addr = remote_addr;
      conninfo->client_addr = local_addr;
   }
   return 0;
}

static int exchange_sig(int sockfd) 
{
   uint32_t sig = SIG;
   ssize_t result;

   debug_printf("Sending sig %x on network\n", sig);
   result = reliable_write(sockfd, &sig, sizeof(sig));
   if (result != (ssize_t)sizeof(sig)) {
      debug_printf("Problem writing sig on network\n");
      return HSHAKE_DROP_CONNECTION;
   }

   debug_printf("Receiving sig from network\n");
   result = reliable_read(sockfd, &sig, sizeof(sig));
   if (result != (ssize_t)sizeof(sig)) {
      debug_printf("Problem reading sig from network\n");
      return result;
   }
   if (sig != SIG) {
      error_printf("Signature %x doesn't match expected value %x\n", sig, SIG);
      return HSHAKE_DROP_CONNECTION;
   }

   return 0;
}

static int send_packet(int sockfd, unsigned char *packet, unsigned int packet_size)
{
   uint32_t size;
   ssize_t result;

   debug_printf("Sending packet size on network\n");
   size = packet_size;
   result = reliable_write(sockfd, &size, sizeof(size));
   if (result != (ssize_t)sizeof(size)) {
      debug_printf("Problem writing packet size on network\n");
      return HSHAKE_INTERNAL_ERROR;
   }

   debug_printf("Sending packet on network\n");
   result = reliable_write(sockfd, packet, packet_size);
   if (result != (ssize_t)packet_size) {
      debug_printf("Problem writing packet on network\n");
      return HSHAKE_INTERNAL_ERROR;
   }   
   
   return 0;
}

static int recv_packet(int sockfd, unsigned char **packet, size_t *packet_size)
{
   uint32_t size;
   ssize_t result;

   debug_printf("Receiving packet size from network\n");
   result = reliable_read(sockfd, &size, sizeof(size));
   if (result != (ssize_t)sizeof(size)) {
      debug_printf("Error reading packet size from network\n");
      return result;
   }
   debug_printf("Received packet size %u\n", (unsigned int) size);
   if (size > 0x100000) {
      error_printf("Received packet of unreasonable size.\n");
      return HSHAKE_DROP_CONNECTION;
   }
   *packet = malloc(size);
   assert(*packet);
   result = reliable_read(sockfd, *packet, size);
   if (result != (ssize_t)size) {
      debug_printf("Error reading packet from network\n");
      return result;
   }
   *packet_size = size;
   debug_printf("Received packet from network\n");   

   return 0;
}

static char *sockaddr_str(struct sockaddr *addr, char *buffer, size_t buffer_size)
{
   switch (addr->sa_family) {
      case AF_INET: {
         struct sockaddr_in *addr_in = (struct sockaddr_in *) addr;
         uint32_t ip = addr_in->sin_addr.s_addr;
         uint16_t port = ntohs(addr_in->sin_port);
         unsigned char *ip_bytes = (unsigned char *) &ip;
         snprintf(buffer, buffer_size, "%u.%u.%u.%u:%u", 
                  (unsigned) ip_bytes[0], (unsigned) ip_bytes[1],
                  (unsigned) ip_bytes[2], (unsigned) ip_bytes[3],
                  port);
         break;
      }
      default:
         snprintf(buffer, buffer_size, "UNKNOWN SOCKADDR TYPE %d", (int) addr->sa_family);
         break;
   }
   return buffer;
}

static int log_error(const char *format, ...)
{
   char *buffer;
   va_list ap;

   va_start(ap, format);
   (void)! vasprintf(&buffer, format, ap);
   va_end(ap);

   assert(buffer);
   if (last_error_message) {
      free(last_error_message);
   }
   last_error_message = buffer;
   return 0;
}

static int log_security_error(const char *format, ...)
{
   char *message_str;
   char *conn_str;
   char server_name[64], client_name[64];
   int message_len;
   va_list ap;

   va_start(ap, format);
   (void)! vasprintf(&message_str, format, ap); 
   va_end(ap);
   
   assert(saved_conninfo);
   if (saved_conninfo->i_am_server)
      (void)! asprintf(&conn_str,
                       "COBO/PMGR Handshake Security Error. My uid = %d. "
                       "Client at %s tried to connect to me at %s, but failed with error: ", 
                       getuid(),
                       sockaddr_str(&saved_conninfo->client_addr, client_name, sizeof(client_name)), 
                       sockaddr_str(&saved_conninfo->server_addr, server_name, sizeof(server_name)));
   else 
      (void)! asprintf(&conn_str,
                       "COBO/PMGR Handshake Security Error. My uid = %d. "
                       "Server at %s took my connection from %s, but failed with error: ", 
                       getuid(),
                       sockaddr_str(&saved_conninfo->server_addr, server_name, sizeof(server_name)), 
                       sockaddr_str(&saved_conninfo->client_addr, client_name, sizeof(client_name)));
   
   if (last_security_message != NULL) {
      free(last_security_message);
   }
   message_len = strlen(conn_str) + strlen(message_str) + 1;
   last_security_message = malloc(message_len);
   assert(last_security_message);
   snprintf(last_security_message, message_len, "%s%s", conn_str, message_str);

   free(message_str);
   free(conn_str);

   return 0;
}
