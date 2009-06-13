/* $Id$
 *
 * Network Performance Meter
 * Copyright (C) 2009 by Thomas Dreibholz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contact: dreibh@iem.uni-due.de
 */

#include "messagereader.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <iostream>

// #define DEBUG_MESSAGEREADER


// ###### Constructor #######################################################
MessageReader::MessageReader()
{
}


// ###### Destructor ########################################################
MessageReader::~MessageReader()
{
}


// ###### Register a socket #################################################
bool MessageReader::registerSocket(const int    protocol,
                                   const int    sd,
                                   const size_t maxMessageSize)
{
   std::map<int, Socket*>::iterator found = SocketMap.find(sd);
   if(found == SocketMap.end()) {
      assert(maxMessageSize >= sizeof(TLVHeader));

      Socket* socket = new Socket;
      assert(socket != NULL);
      socket->MessageBuffer = new char[maxMessageSize];
      assert(socket->MessageBuffer != NULL);
      socket->MessageBufferSize = maxMessageSize;
      socket->MessageSize       = 0;
      socket->BytesRead         = 0;
      socket->Status            = Socket::MRS_WaitingForHeader;
      socket->Protocol          = protocol;
      socket->SocketDescriptor  = sd;
      SocketMap.insert(std::pair<int, Socket*>(sd, socket));
   }
   else {
      std::cerr << "ERROR: Socket registered twice!" << std::endl;
      return(false);
   }
   return(true);
}


// ###### Deregister a socket ###############################################
bool MessageReader::deregisterSocket(const int sd)
{
   std::map<int, Socket*>::iterator found = SocketMap.find(sd);
   if(found != SocketMap.end()) {
      Socket* socket = found->second;
      SocketMap.erase(found);
      delete [] socket->MessageBuffer;
      delete socket;
   }
   else {
      std::cerr << "ERROR: Socket is not registered!" << std::endl;
      return(false);
   }
   return(true);
}


// ###### Receive full message ##############################################
ssize_t MessageReader::receiveMessage(const int        sd,
                                      void*            buffer,
                                      size_t           bufferSize,
                                      sockaddr*        from,
                                      socklen_t*       fromSize,
                                      sctp_sndrcvinfo* sinfo,
                                      int*             msgFlags)
{
   Socket* socket = getSocket(sd);
   if(socket != NULL) {
      // ====== Find out the number of bytes to read ========================
      ssize_t received;
      size_t  bytesToRead;
      if( (socket->Protocol == IPPROTO_SCTP) || (socket->Protocol == IPPROTO_TCP) ) {
         // SCTP and TCP can return partial messages upon recv() calls. TCP
         // may event return multiple messages, if the buffer size is large enough!
         if(socket->Status == Socket::MRS_WaitingForHeader) {
            assert(sizeof(TLVHeader) >= socket->BytesRead);
            bytesToRead = sizeof(TLVHeader) - socket->BytesRead;
         }
         else if(socket->Status == Socket::MRS_PartialRead) {
            bytesToRead = socket->MessageSize - socket->BytesRead;
         }
         else {
            return(MRRM_STREAM_ERROR);
         }
         assert(bytesToRead + socket->BytesRead <= socket->MessageBufferSize);
      }
      else {
         // DCCP and UDP will always return only a single message on recv() calls.
         bytesToRead = socket->MessageBufferSize;
      }


      // ====== Read from socket ============================================
      int dummyFlags;
      if(msgFlags == NULL) {
         dummyFlags = 0;
         msgFlags   = &dummyFlags;
      }
      if(socket->Protocol == IPPROTO_SCTP) {
         received = sctp_recvmsg(socket->SocketDescriptor,
                                 (char*)&socket->MessageBuffer[socket->BytesRead], bytesToRead,
                                 from, fromSize, sinfo, msgFlags);
      }
      else {
         received = ext_recvfrom(socket->SocketDescriptor,
                                 (char*)&socket->MessageBuffer[socket->BytesRead], bytesToRead,
                                 *msgFlags, from, fromSize);
      }


      // ====== Handle received data ========================================
      if(received > 0) {
         socket->BytesRead += (size_t)received;
         // ====== Handle message header ====================================
         if(socket->Status == Socket::MRS_WaitingForHeader) {
            // ====== Handle SCTP notification header =======================
            if((socket->Protocol == IPPROTO_SCTP) && (*msgFlags & MSG_NOTIFICATION)) {
               socket->MessageSize = sizeof(sctp_notification);   // maximum length
               socket->Status      = Socket::MRS_PartialRead;
               // SCTP notification has no TLV header, but must be handled like
               // a message. The actual length of the notification is unknown, we
               // need to look for MSG_EOF!
            }
            // ====== Handle TLV header =====================================
            else {
               if(socket->BytesRead >= sizeof(TLVHeader)) {
                  const TLVHeader* header = (const TLVHeader*)socket->MessageBuffer;
#ifdef DEBUG_MESSAGEREADER
                  printf("Socket %d:   T=%u F=%02x L=%u   [Header]\n",
                          socket->SocketDescriptor,
                          (unsigned int)header->Type, (unsigned int)header->Flags, ntohs(header->Length));
#endif
                  socket->MessageSize = ntohs(header->Length);
                  if(socket->MessageSize < sizeof(TLVHeader)) {
                     std::cerr << "ERROR: Message size < TLV size!" << std::endl;
                     socket->Status = Socket::MRS_StreamError;
                     return(MRRM_STREAM_ERROR);
                  }
                  else if(socket->MessageSize > socket->MessageBufferSize) {
                     std::cerr << "ERROR: Message too large to fit buffer!" << std::endl;
                     socket->Status = Socket::MRS_StreamError;
                     return(MRRM_STREAM_ERROR);
                  }
                  socket->Status = Socket::MRS_PartialRead;
               }
            }
            // Continue here with MRS_PartialRead status!
            // (will return MRRM_PARTIAL_READ, or message on header-only message)
         }

         // ====== Handle message payload ===================================
         if(socket->Status == Socket::MRS_PartialRead) {
#ifdef DEBUG_MESSAGEREADER
            printf("Socket %d:   T=%u F=%02x L=%u   [%u/%u]\n",
                   socket->SocketDescriptor,
                   ((const TLVHeader*)socket->MessageBuffer)->Type,
                   ((const TLVHeader*)socket->MessageBuffer)->Flags,
                   ntohs(((const TLVHeader*)socket->MessageBuffer)->Length),
                   (unsigned int)socket->BytesRead,
                   (unsigned int)socket->MessageSize);
#endif

            // ====== Partially read message ================================
            if(socket->BytesRead < socket->MessageSize) {
               if(socket->Protocol == IPPROTO_SCTP) {
                  if(*msgFlags & MSG_EOR) {   // end of SCTP message
                     if(!(*msgFlags & MSG_NOTIFICATION)) {   // data message
                        std::cerr << "ERROR: SCTP message end before TLV message end!" << std::endl
                                  << "       Read " << socket->BytesRead
                                  << ", expected " << socket->MessageSize << std::endl;
                        socket->Status = Socket::MRS_StreamError;
                        return(MRRM_STREAM_ERROR);
                     }
                     // This is the end of the SCTP notification. The message
                     // is complete here. Return it to the caller.
                     socket->MessageSize = socket->BytesRead;
                  }
                  else {
                     return(MRRM_PARTIAL_READ);
                  }
               }
               else {
                  return(MRRM_PARTIAL_READ);
               }
            }

            // ====== Completed reading =====================================
            if(socket->MessageSize > bufferSize) {
               std::cerr << "ERROR: Buffer size for MessageReader::receiveMessage() is too small!" << std::endl;
               socket->Status = Socket::MRS_StreamError;
               return(MRRM_STREAM_ERROR);
            }
            if((socket->Protocol == IPPROTO_SCTP) && (!(*msgFlags & MSG_EOR))) {
               std::cerr << "ERROR: TLV message end does not match with SCTP message end!" << std::endl;
               socket->Status = Socket::MRS_StreamError;
               return(MRRM_STREAM_ERROR);
            }
            received = socket->MessageSize;
            memcpy(buffer, socket->MessageBuffer, socket->MessageSize);
            socket->Status      = Socket::MRS_WaitingForHeader;
            socket->MessageSize = 0;
            socket->BytesRead   = 0;
            return(received);
         }
      }
      // ====== Handle read errors ==========================================
      else if(received < 0) {
         return(MRRM_SOCKET_ERROR);
      }
   }
   return(MRRM_BAD_SOCKET);
}