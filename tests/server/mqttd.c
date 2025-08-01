/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "first.h"

#include <stdlib.h>
#include <string.h>

/* Function
 *
 * Accepts a TCP connection on a custom port (IPv4 or IPv6).  Speaks MQTT.
 *
 * Read commands from FILE (set with --config). The commands control how to
 * act and is reset to defaults each client TCP connect.
 *
 */

/* based on sockfilt.c */

#define MQTT_MSG_CONNECT    0x10
#define MQTT_MSG_CONNACK    0x20
#define MQTT_MSG_PUBLISH    0x30
/* #define MQTT_MSG_PUBACK     0x40 */
#define MQTT_MSG_SUBSCRIBE  0x82
#define MQTT_MSG_SUBACK     0x90
#define MQTT_MSG_DISCONNECT 0xe0

struct mqttd_configurable {
  unsigned char version; /* initial version byte in the request must match
                            this */
  bool publish_before_suback;
  bool short_publish;
  bool excessive_remaining;
  unsigned char error_connack;
  int testnum;
};

#define REQUEST_DUMP  "server.input"
#define CONFIG_VERSION 5

static struct mqttd_configurable m_config;

static void mqttd_resetdefaults(void)
{
  logmsg("Reset to defaults");
  m_config.version = CONFIG_VERSION;
  m_config.publish_before_suback = FALSE;
  m_config.short_publish = FALSE;
  m_config.excessive_remaining = FALSE;
  m_config.error_connack = 0;
  m_config.testnum = 0;
}

static void mqttd_getconfig(void)
{
  FILE *fp = fopen(configfile, FOPEN_READTEXT);
  mqttd_resetdefaults();
  if(fp) {
    char buffer[512];
    logmsg("parse config file");
    while(fgets(buffer, sizeof(buffer), fp)) {
      char key[32];
      char value[32];
      if(sscanf(buffer, "%31s %31s", key, value) == 2) {
        if(!strcmp(key, "version")) {
          m_config.version = byteval(value);
          logmsg("version [%d] set", m_config.version);
        }
        else if(!strcmp(key, "PUBLISH-before-SUBACK")) {
          logmsg("PUBLISH-before-SUBACK set");
          m_config.publish_before_suback = TRUE;
        }
        else if(!strcmp(key, "short-PUBLISH")) {
          logmsg("short-PUBLISH set");
          m_config.short_publish = TRUE;
        }
        else if(!strcmp(key, "error-CONNACK")) {
          m_config.error_connack = byteval(value);
          logmsg("error-CONNACK = %d", m_config.error_connack);
        }
        else if(!strcmp(key, "Testnum")) {
          m_config.testnum = atoi(value);
          logmsg("testnum = %d", m_config.testnum);
        }
        else if(!strcmp(key, "excessive-remaining")) {
          logmsg("excessive-remaining set");
          m_config.excessive_remaining = TRUE;
        }
      }
    }
    fclose(fp);
  }
  else {
    logmsg("No config file '%s' to read", configfile);
  }
}

typedef enum {
  FROM_CLIENT,
  FROM_SERVER
} mqttdir;

static void logprotocol(mqttdir dir,
                        const char *prefix, size_t remlen,
                        FILE *output,
                        unsigned char *buffer, ssize_t len)
{
  char data[12000] = "";
  ssize_t i;
  unsigned char *ptr = buffer;
  char *optr = data;
  int left = sizeof(data);

  for(i = 0; i < len && (left >= 0); i++) {
    snprintf(optr, left, "%02x", ptr[i]);
    optr += 2;
    left -= 2;
  }
  fprintf(output, "%s %s %x %s\n",
          dir == FROM_CLIENT ? "client" : "server",
          prefix, (int)remlen, data);
}


/* return 0 on success */
static int connack(FILE *dump, curl_socket_t fd)
{
  unsigned char packet[]={
    MQTT_MSG_CONNACK, 0x02,
    0x00, 0x00
  };
  ssize_t rc;

  packet[3] = m_config.error_connack;

  rc = swrite(fd, (char *)packet, sizeof(packet));
  if(rc > 0) {
    logmsg("WROTE %zd bytes [CONNACK]", rc);
    loghex(packet, rc);
    logprotocol(FROM_SERVER, "CONNACK", 2, dump, packet, sizeof(packet));
  }
  if(rc == sizeof(packet)) {
    return 0;
  }
  return 1;
}

/* return 0 on success */
static int suback(FILE *dump, curl_socket_t fd, unsigned short packetid)
{
  unsigned char packet[]={
    MQTT_MSG_SUBACK, 0x03,
    0, 0, /* filled in below */
    0x00
  };
  ssize_t rc;
  packet[2] = (unsigned char)(packetid >> 8);
  packet[3] = (unsigned char)(packetid & 0xff);

  rc = swrite(fd, (char *)packet, sizeof(packet));
  if(rc == sizeof(packet)) {
    logmsg("WROTE %zd bytes [SUBACK]", rc);
    loghex(packet, rc);
    logprotocol(FROM_SERVER, "SUBACK", 3, dump, packet, rc);
    return 0;
  }
  return 1;
}

#ifdef QOS
/* return 0 on success */
static int puback(FILE *dump, curl_socket_t fd, unsigned short packetid)
{
  unsigned char packet[]={
    MQTT_MSG_PUBACK, 0x00,
    0, 0 /* filled in below */
  };
  ssize_t rc;
  packet[2] = (unsigned char)(packetid >> 8);
  packet[3] = (unsigned char)(packetid & 0xff);

  rc = swrite(fd, (char *)packet, sizeof(packet));
  if(rc == sizeof(packet)) {
    logmsg("WROTE %zd bytes [PUBACK]", rc);
    loghex(packet, rc);
    logprotocol(FROM_SERVER, dump, packet, rc);
    return 0;
  }
  logmsg("Failed sending [PUBACK]");
  return 1;
}
#endif

/* return 0 on success */
static int disconnect(FILE *dump, curl_socket_t fd)
{
  unsigned char packet[]={
    MQTT_MSG_DISCONNECT, 0x00,
  };
  ssize_t rc = swrite(fd, (char *)packet, sizeof(packet));
  if(rc == sizeof(packet)) {
    logmsg("WROTE %zd bytes [DISCONNECT]", rc);
    loghex(packet, rc);
    logprotocol(FROM_SERVER, "DISCONNECT", 0, dump, packet, rc);
    return 0;
  }
  logmsg("Failed sending [DISCONNECT]");
  return 1;
}

/*
  do
     encodedByte = X MOD 128

     X = X DIV 128

     // if there are more data to encode, set the top bit of this byte

     if ( X > 0 )

        encodedByte = encodedByte OR 128

      endif

    'output' encodedByte

  while ( X > 0 )

*/

/* return number of bytes used */
static size_t encode_length(size_t packetlen,
                            unsigned char *remlength) /* 4 bytes */
{
  size_t bytes = 0;
  unsigned char encode;

  do {
    encode = packetlen % 0x80;
    packetlen /= 0x80;
    if(packetlen)
      encode |= 0x80;

    remlength[bytes++] = encode;

    if(bytes > 3) {
      logmsg("too large packet!");
      return 0;
    }
  } while(packetlen);

  return bytes;
}


static size_t decode_length(unsigned char *buffer,
                            size_t buflen, size_t *lenbytes)
{
  size_t len = 0;
  size_t mult = 1;
  size_t i;
  unsigned char encoded = 0x80;

  for(i = 0; (i < buflen) && (encoded & 0x80); i++) {
    encoded = buffer[i];
    len += (encoded & 0x7f) * mult;
    mult *= 0x80;
  }

  if(lenbytes)
    *lenbytes = i;

  return len;
}


/* return 0 on success */
static int publish(FILE *dump,
                   curl_socket_t fd, unsigned short packetid,
                   char *topic, const char *payload, size_t payloadlen)
{
  size_t topiclen = strlen(topic);
  unsigned char *packet;
  size_t payloadindex;
  size_t remaininglength = topiclen + 2 + payloadlen;
  size_t packetlen;
  size_t sendamount;
  ssize_t rc;
  unsigned char rembuffer[4];
  size_t encodedlen;

  if(m_config.excessive_remaining) {
    /* manually set illegal remaining length */
    rembuffer[0] = 0xff;
    rembuffer[1] = 0xff;
    rembuffer[2] = 0xff;
    rembuffer[3] = 0x80; /* maximum allowed here by spec is 0x7f */
    encodedlen = 4;
  }
  else
    encodedlen = encode_length(remaininglength, rembuffer);

  /* one packet type byte (possibly two more for packetid) */
  packetlen = remaininglength + encodedlen + 1;
  packet = malloc(packetlen);
  if(!packet)
    return 1;

  packet[0] = MQTT_MSG_PUBLISH;
  memcpy(&packet[1], rembuffer, encodedlen);

  (void)packetid;
  /* packet_id if QoS is set */

  packet[1 + encodedlen] = (unsigned char)(topiclen >> 8);
  packet[2 + encodedlen] = (unsigned char)(topiclen & 0xff);
  memcpy(&packet[3 + encodedlen], topic, topiclen);

  payloadindex = 3 + topiclen + encodedlen;
  memcpy(&packet[payloadindex], payload, payloadlen);

  sendamount = packetlen;
  if(m_config.short_publish)
    sendamount -= 2;

  rc = swrite(fd, (char *)packet, sendamount);
  if(rc > 0) {
    logmsg("WROTE %zd bytes [PUBLISH]", rc);
    loghex(packet, rc);
    logprotocol(FROM_SERVER, "PUBLISH", remaininglength, dump, packet, rc);
  }
  free(packet);
  if((size_t)rc == packetlen)
    return 0;
  return 1;
}

#define MAX_TOPIC_LENGTH 65535
#define MAX_CLIENT_ID_LENGTH 32

static char topic[MAX_TOPIC_LENGTH + 1];

static int fixedheader(curl_socket_t fd,
                       unsigned char *bytep,
                       size_t *remaining_lengthp,
                       size_t *remaining_length_bytesp)
{
  /* get the fixed header */
  unsigned char buffer[10];

  /* get the first two bytes */
  ssize_t rc = sread(fd, (char *)buffer, 2);
  size_t i;
  if(rc < 2) {
    logmsg("READ %zd bytes [SHORT!]", rc);
    return 1; /* fail */
  }
  logmsg("READ %zd bytes", rc);
  loghex(buffer, rc);
  *bytep = buffer[0];

  /* if the length byte has the top bit set, get the next one too */
  i = 1;
  while(buffer[i] & 0x80) {
    i++;
    rc = sread(fd, (char *)&buffer[i], 1);
    if(rc != 1) {
      logmsg("Remaining Length broken");
      return 1;
    }
  }
  *remaining_lengthp = decode_length(&buffer[1], i, remaining_length_bytesp);
  logmsg("Remaining Length: %zu [%zu bytes]", *remaining_lengthp,
         *remaining_length_bytesp);
  return 0;
}

static curl_socket_t mqttit(curl_socket_t fd)
{
  size_t buff_size = 10*1024;
  unsigned char *buffer = NULL;
  ssize_t rc;
  unsigned char byte;
  unsigned short packet_id;
  size_t payload_len;
  size_t client_id_length;
  size_t topic_len;
  size_t remaining_length = 0;
  size_t bytes = 0; /* remaining length field size in bytes */
  char client_id[MAX_CLIENT_ID_LENGTH];
  long testno;
  FILE *stream = NULL;
  FILE *dump;
  char dumpfile[256];

  static const char protocol[7] = {
    0x00, 0x04,       /* protocol length */
    'M','Q','T','T',  /* protocol name */
    0x04              /* protocol level */
  };
  snprintf(dumpfile, sizeof(dumpfile), "%s/%s", logdir, REQUEST_DUMP);
  dump = fopen(dumpfile, "ab");
  if(!dump)
    goto end;

  mqttd_getconfig();

  testno = m_config.testnum;

  if(testno)
    logmsg("Found test number %ld", testno);

  buffer = malloc(buff_size);
  if(!buffer) {
    logmsg("Out of memory, unable to allocate buffer");
    goto end;
  }
  memset(buffer, 0, buff_size);

  do {
    unsigned char usr_flag = 0x80;
    unsigned char passwd_flag = 0x40;
    unsigned char conn_flags;
    const size_t client_id_offset = 12;
    size_t start_usr;
    size_t start_passwd;

    /* get the fixed header */
    rc = fixedheader(fd, &byte, &remaining_length, &bytes);
    if(rc)
      break;

    if(remaining_length >= buff_size) {
      unsigned char *newbuffer;
      buff_size = remaining_length;
      newbuffer = realloc(buffer, buff_size);
      if(!newbuffer) {
        logmsg("Failed realloc of size %zu", buff_size);
        goto end;
      }
      buffer = newbuffer;
    }

    if(remaining_length) {
      /* reading variable header and payload into buffer */
      rc = sread(fd, (char *)buffer, remaining_length);
      if(rc > 0) {
        logmsg("READ %zd bytes", rc);
        loghex(buffer, rc);
      }
    }

    if(byte == MQTT_MSG_CONNECT) {
      logprotocol(FROM_CLIENT, "CONNECT", remaining_length,
                  dump, buffer, rc);

      if(memcmp(protocol, buffer, sizeof(protocol))) {
        logmsg("Protocol preamble mismatch");
        goto end;
      }
      /* ignore the connect flag byte and two keepalive bytes */
      payload_len = (size_t)(buffer[10] << 8) | buffer[11];
      /* first part of the payload is the client ID */
      client_id_length = payload_len;

      /* checking if user and password flags were set */
      conn_flags = buffer[7];

      start_usr = client_id_offset + payload_len;
      if(usr_flag == (unsigned char)(conn_flags & usr_flag)) {
        logmsg("User flag is present in CONN flag");
        payload_len += (size_t)(buffer[start_usr] << 8) |
                       buffer[start_usr + 1];
        payload_len += 2; /* MSB and LSB for user length */
      }

      start_passwd = client_id_offset + payload_len;
      if(passwd_flag == (char)(conn_flags & passwd_flag)) {
        logmsg("Password flag is present in CONN flags");
        payload_len += (size_t)(buffer[start_passwd] << 8) |
                       buffer[start_passwd + 1];
        payload_len += 2; /* MSB and LSB for password length */
      }

      /* check the length of the payload */
      if((ssize_t)payload_len != (rc - 12)) {
        logmsg("Payload length mismatch, expected %zx got %zx",
               rc - 12, payload_len);
        goto end;
      }
      /* check the length of the client ID */
      else if((client_id_length + 1) > MAX_CLIENT_ID_LENGTH) {
        logmsg("Too large client id");
        goto end;
      }
      memcpy(client_id, &buffer[12], client_id_length);
      client_id[client_id_length] = 0;

      logmsg("MQTT client connect accepted: %s", client_id);

      /* The first packet sent from the Server to the Client MUST be a
         CONNACK Packet */

      if(connack(dump, fd)) {
        logmsg("failed sending CONNACK");
        goto end;
      }
    }
    else if(byte == MQTT_MSG_SUBSCRIBE) {
      int error;
      char *data;
      size_t datalen;
      logprotocol(FROM_CLIENT, "SUBSCRIBE", remaining_length,
                  dump, buffer, rc);
      logmsg("Incoming SUBSCRIBE");

      if(rc < 6) {
        logmsg("Too small SUBSCRIBE");
        goto end;
      }

      /* two bytes packet id */
      packet_id = (unsigned short)((buffer[0] << 8) | buffer[1]);

      /* two bytes topic length */
      topic_len = (size_t)(buffer[2] << 8) | buffer[3];
      if(topic_len != (remaining_length - 5)) {
        logmsg("Wrong topic length, got %zu expected %zu",
               topic_len, remaining_length - 5);
        goto end;
      }
      memcpy(topic, &buffer[4], topic_len);
      topic[topic_len] = 0;

      /* there's a QoS byte (two bits) after the topic */

      logmsg("SUBSCRIBE to '%s' [%d]", topic, packet_id);
      stream = test2fopen(testno, logdir);
      if(!stream) {
        error = errno;
        logmsg("fopen() failed with error (%d) %s", error, strerror(error));
        logmsg("Couldn't open test file %ld", testno);
        goto end;
      }
      error = getpart(&data, &datalen, "reply", "data", stream);
      if(!error) {
        if(!m_config.publish_before_suback) {
          if(suback(dump, fd, packet_id)) {
            logmsg("failed sending SUBACK");
            free(data);
            goto end;
          }
        }
        if(publish(dump, fd, packet_id, topic, data, datalen)) {
          logmsg("PUBLISH failed");
          free(data);
          goto end;
        }
        free(data);
        if(m_config.publish_before_suback) {
          if(suback(dump, fd, packet_id)) {
            logmsg("failed sending SUBACK");
            goto end;
          }
        }
      }
      else {
        const char *def = "this is random payload yes yes it is";
        publish(dump, fd, packet_id, topic, def, strlen(def));
      }
      disconnect(dump, fd);
    }
    else if((byte & 0xf0) == (MQTT_MSG_PUBLISH & 0xf0)) {
      size_t topiclen;

      logmsg("Incoming PUBLISH");
      logprotocol(FROM_CLIENT, "PUBLISH", remaining_length,
                  dump, buffer, rc);

      topiclen = (size_t)(buffer[1 + bytes] << 8) | buffer[2 + bytes];
      logmsg("Got %zu bytes topic", topiclen);

#ifdef QOS
      /* Handle packetid if there is one. Send puback if QoS > 0 */
      puback(dump, fd, 0);
#endif
      /* expect a disconnect here */
      /* get the request */
      rc = sread(fd, (char *)&buffer[0], 2);

      logmsg("READ %zd bytes [DISCONNECT]", rc);
      loghex(buffer, rc);
      logprotocol(FROM_CLIENT, "DISCONNECT", 0, dump, buffer, rc);
      goto end;
    }
    else {
      /* not supported (yet) */
      goto end;
    }
  } while(1);

end:
  if(buffer)
    free(buffer);
  if(dump)
    fclose(dump);
  if(stream)
    fclose(stream);
  return CURL_SOCKET_BAD;
}

/*
  sockfdp is a pointer to an established stream or CURL_SOCKET_BAD

  if sockfd is CURL_SOCKET_BAD, listendfd is a listening socket we must
  accept()
*/
static bool mqttd_incoming(curl_socket_t listenfd)
{
  fd_set fds_read;
  fd_set fds_write;
  fd_set fds_err;
  int clients = 0; /* connected clients */

  if(got_exit_signal) {
    logmsg("signalled to die, exiting...");
    return FALSE;
  }

#ifdef HAVE_GETPPID
  /* As a last resort, quit if socks5 process becomes orphan. */
  if(getppid() <= 1) {
    logmsg("process becomes orphan, exiting");
    return FALSE;
  }
#endif

  do {
    ssize_t rc;
    int error = 0;
    curl_socket_t sockfd = listenfd;
    int maxfd = (int)sockfd;

    FD_ZERO(&fds_read);
    FD_ZERO(&fds_write);
    FD_ZERO(&fds_err);

    /* there's always a socket to wait for */
#ifdef __DJGPP__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warith-conversion"
#endif
    FD_SET(sockfd, &fds_read);
#ifdef __DJGPP__
#pragma GCC diagnostic pop
#endif

    do {
      /* select() blocking behavior call on blocking descriptors please */
      rc = select(maxfd + 1, &fds_read, &fds_write, &fds_err, NULL);
      if(got_exit_signal) {
        logmsg("signalled to die, exiting...");
        return FALSE;
      }
    } while((rc == -1) && ((error = SOCKERRNO) == SOCKEINTR));

    if(rc < 0) {
      logmsg("select() failed with error (%d) %s",
             error, strerror(error));
      return FALSE;
    }

    if(FD_ISSET(sockfd, &fds_read)) {
      curl_socket_t newfd = accept(sockfd, NULL, NULL);
      if(CURL_SOCKET_BAD == newfd) {
        error = SOCKERRNO;
        logmsg("accept() failed with error (%d) %s", error, sstrerror(error));
      }
      else {
        logmsg("====> Client connect, fd %ld. "
               "Read config from %s", (long)newfd, configfile);
        set_advisor_read_lock(loglockfile);
        (void)mqttit(newfd); /* until done */
        clear_advisor_read_lock(loglockfile);

        logmsg("====> Client disconnect");
        sclose(newfd);
      }
    }
  } while(clients);

  return TRUE;
}

static int test_mqttd(int argc, char *argv[])
{
  curl_socket_t sock = CURL_SOCKET_BAD;
  curl_socket_t msgsock = CURL_SOCKET_BAD;
  int wrotepidfile = 0;
  int wroteportfile = 0;
  bool juggle_again;
  int error;
  int arg = 1;

  pidname = ".mqttd.pid";
  portname = ".mqttd.port";
  serverlogfile = "log/mqttd.log";
  configfile = "mqttd.config";
  server_port = 1883; /* MQTT default port */

  while(argc > arg) {
    if(!strcmp("--version", argv[arg])) {
      printf("mqttd IPv4%s\n",
#ifdef USE_IPV6
             "/IPv6"
#else
             ""
#endif
             );
      return 0;
    }
    else if(!strcmp("--pidfile", argv[arg])) {
      arg++;
      if(argc > arg)
        pidname = argv[arg++];
    }
    else if(!strcmp("--portfile", argv[arg])) {
      arg++;
      if(argc > arg)
        portname = argv[arg++];
    }
    else if(!strcmp("--config", argv[arg])) {
      arg++;
      if(argc > arg)
        configfile = argv[arg++];
    }
    else if(!strcmp("--logfile", argv[arg])) {
      arg++;
      if(argc > arg)
        serverlogfile = argv[arg++];
    }
    else if(!strcmp("--logdir", argv[arg])) {
      arg++;
      if(argc > arg)
        logdir = argv[arg++];
    }
    else if(!strcmp("--ipv6", argv[arg])) {
#ifdef USE_IPV6
      socket_domain = AF_INET6;
      ipv_inuse = "IPv6";
#endif
      arg++;
    }
    else if(!strcmp("--ipv4", argv[arg])) {
      /* for completeness, we support this option as well */
#ifdef USE_IPV6
      socket_domain = AF_INET;
      ipv_inuse = "IPv4";
#endif
      arg++;
    }
    else if(!strcmp("--port", argv[arg])) {
      arg++;
      if(argc > arg) {
        char *endptr;
        unsigned long ulnum = strtoul(argv[arg], &endptr, 10);
        if((endptr != argv[arg] + strlen(argv[arg])) ||
           ((ulnum != 0UL) && ((ulnum < 1025UL) || (ulnum > 65535UL)))) {
          fprintf(stderr, "mqttd: invalid --port argument (%s)\n",
                  argv[arg]);
          return 0;
        }
        server_port = util_ultous(ulnum);
        arg++;
      }
    }
    else {
      puts("Usage: mqttd [option]\n"
           " --config [file]\n"
           " --version\n"
           " --logfile [file]\n"
           " --logdir [directory]\n"
           " --pidfile [file]\n"
           " --portfile [file]\n"
           " --ipv4\n"
           " --ipv6\n"
           " --port [port]\n");
      return 0;
    }
  }

  snprintf(loglockfile, sizeof(loglockfile), "%s/%s/mqtt-%s.lock",
           logdir, SERVERLOGS_LOCKDIR, ipv_inuse);

#ifdef _WIN32
  if(win32_init())
    return 2;
#endif

  CURLX_SET_BINMODE(stdin);
  CURLX_SET_BINMODE(stdout);
  CURLX_SET_BINMODE(stderr);

  install_signal_handlers(FALSE);

  sock = socket(socket_domain, SOCK_STREAM, 0);

  if(CURL_SOCKET_BAD == sock) {
    error = SOCKERRNO;
    logmsg("Error creating socket (%d) %s", error, sstrerror(error));
    goto mqttd_cleanup;
  }

  {
    /* passive daemon style */
    sock = sockdaemon(sock, &server_port, NULL, FALSE);
    if(CURL_SOCKET_BAD == sock) {
      goto mqttd_cleanup;
    }
    msgsock = CURL_SOCKET_BAD; /* no stream socket yet */
  }

  logmsg("Running %s version", ipv_inuse);
  logmsg("Listening on port %hu", server_port);

  wrotepidfile = write_pidfile(pidname);
  if(!wrotepidfile) {
    goto mqttd_cleanup;
  }

  wroteportfile = write_portfile(portname, server_port);
  if(!wroteportfile) {
    goto mqttd_cleanup;
  }

  do {
    juggle_again = mqttd_incoming(sock);
  } while(juggle_again);

mqttd_cleanup:

  if((msgsock != sock) && (msgsock != CURL_SOCKET_BAD))
    sclose(msgsock);

  if(sock != CURL_SOCKET_BAD)
    sclose(sock);

  if(wrotepidfile)
    unlink(pidname);
  if(wroteportfile)
    unlink(portname);

  restore_signal_handlers(FALSE);

  if(got_exit_signal) {
    logmsg("============> mqttd exits with signal (%d)", exit_signal);
    /*
     * To properly set the return status of the process we
     * must raise the same signal SIGINT or SIGTERM that we
     * caught and let the old handler take care of it.
     */
    raise(exit_signal);
  }

  logmsg("============> mqttd quits");
  return 0;
}
