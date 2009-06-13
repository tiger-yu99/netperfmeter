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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <math.h>
#include <assert.h>
#include <bzlib.h>

#include <iostream>
#include <vector>
#include <set>

#include "tools.h"
#include "flowspec.h"
#include "netperfmeterpackets.h"
#include "control.h"
#include "transfer.h"
#include "statisticswriter.h"


#ifndef IPPROTO_DCCP
#warning DCCP is not supported by the API of this system!
#endif


using namespace std;



vector<FlowSpec*> gFlowSet;
set<int>          gConnectedSocketsSet;
int               gControlSocket       = -1;
int               gTCPSocket           = -1;
int               gUDPSocket           = -1;
int               gSCTPSocket          = -1;
int               gDCCPSocket          = -1;
size_t            gMaxMsgSize          = 16000;
double            gRuntime             = -1.0;
bool              gStopTimeReached     = false;
MessageReader     gMessageReader;
StatisticsWriter  gStatisticsWriter;



// ###### Handle global command-line parameter ##############################
bool handleGlobalParameter(const char* parameter)
{
   if(strncmp(parameter, "-maxmsgsize=", 12) == 0) {
      gMaxMsgSize = atol((const char*)&parameter[12]);
      if(gMaxMsgSize > 65536) {
         gMaxMsgSize = 65536;
      }
      else if(gMaxMsgSize < 128) {
         gMaxMsgSize = 128;
      }
      return(true);
   }
   else if(strncmp(parameter, "-runtime=", 9) == 0) {
      gRuntime = atof((const char*)&parameter[9]);
      return(true);
   }
   return(false);
}


// ###### Print global parameter settings ###################################
void printGlobalParameters()
{
   std::cout << "Global Parameters:" << std::endl
             << "   - Runtime          = ";
   if(gRuntime >= 0.0) {
      std::cout << gRuntime << "s" << std::endl;
   }
   else {
      std::cout << "until manual stop" << std::endl;
   }
   std::cout << "   - Max Message Size = " << gMaxMsgSize << std::endl
             << std::endl;
}


// ###### Read random number parameter ######################################
const char* getNextEntry(const char* parameters, double* value, uint8_t* rng)
{
   int n = 0;
   if(sscanf(parameters, "exp%lf%n", value, &n) == 1) {
      *rng = RANDOM_EXPONENTIAL;
   }
   else if(sscanf(parameters, "const%lf%n", value, &n) == 1) {
      *rng = RANDOM_CONSTANT;
   }
   else {
      cerr << "ERROR: Invalid parameters " << parameters << endl;
      exit(1);
   }
   if(parameters[n] == 0x00) {
      return(NULL);
   }
   return((const char*)&parameters[n + 1]);
}


// ###### Read flow option ##################################################
const char* getNextOption(const char* parameters, FlowSpec* flowSpec)
{
   int    n     = 0;
   double value = 0.0;
   char   description[256];

   if(sscanf(parameters, "unordered=%lf%n", &value, &n) == 1) {
      if((value < 0.0) || (value > 1.0)) {
         cerr << "ERROR: Bad probability for \"unordered\" option in " << parameters << "!" << endl;
         exit(1);
      }
      flowSpec->OrderedMode = value;
   }
   else if(sscanf(parameters, "unreliable=%lf%n", &value, &n) == 1) {
      if((value < 0.0) || (value > 1.0)) {
         cerr << "ERROR: Bad probability for \"unreliable\" option in " << parameters << "!" << endl;
         exit(1);
      }
      flowSpec->ReliableMode = value;
   }
   else if(sscanf(parameters, "description=%255[^:]s%n", (char*)&description, &n) == 1) {
      flowSpec->Description = std::string(description);
      n = 12 + strlen(description);
   }
   else if(strncmp(parameters,"onoff=", 6) == 0) {
      unsigned int lastEvent = 0;
      size_t       m         = 5;
      while( (parameters[m] != 0x00) && (parameters[m] != ':') ) {
         m++;
         double       value;
         unsigned int onoff;
         if(sscanf((const char*)&parameters[m], "+%lf%n", &value, &n) == 1) {
            // Relative time
            onoff = (unsigned int)rint(1000.0 * value);
            onoff += lastEvent;
         }
         else if(sscanf((const char*)&parameters[m], "%lf%n", &value, &n) == 1) {
            // Absolute time
            onoff = (unsigned int)rint(1000.0 * value);
         }
         else {
            cerr << "ERROR: Invalid on/off list at " << (const char*)&parameters[m] << "!" << std::endl;
            exit(1);
         }
         flowSpec->OnOffEvents.insert(onoff);
         lastEvent = onoff;
         m += n;
      }
      n = m;
   }
   else {
      cerr << "ERROR: Invalid options " << parameters << "!" << endl;
      exit(1);
   }
   if(parameters[n] == 0x00) {
      return(NULL);
   }
   return((const char*)&parameters[n + 1]);
}


// ###### Create FlowSpec for new flow ######################################
FlowSpec* createLocalFlow(const char*     parameters,
                          const uint8_t   protocol,
                          FlowSpec*       lastFlow,
                          const uint64_t  measurementID,
                          uint32_t*       flowID,
                          const sockaddr* remoteAddress,
                          const sockaddr* controlAddress)
{
   FlowSpec* flowSpec = new FlowSpec;
   assert(flowSpec != NULL);

   flowSpec->MeasurementID = measurementID;
   flowSpec->FlowID        = (*flowID)++;
   flowSpec->Protocol      = protocol;

   char description[32];
   snprintf((char*)&description, sizeof(description), "Flow %u", flowSpec->FlowID);
   flowSpec->Description = std::string(description);

   parameters = getNextEntry(parameters, &flowSpec->OutboundFrameRate, &flowSpec->OutboundFrameRateRng);
   if(parameters) {
      parameters = getNextEntry(parameters, &flowSpec->OutboundFrameSize, &flowSpec->OutboundFrameSizeRng);
      if(parameters) {
         parameters = getNextEntry(parameters, &flowSpec->InboundFrameRate, &flowSpec->InboundFrameRateRng);
         if(parameters) {
            parameters = getNextEntry(parameters, &flowSpec->InboundFrameSize, &flowSpec->InboundFrameSizeRng);
            if(parameters) {
               while( (parameters = getNextOption(parameters, flowSpec)) ) {
               }
            }
         }
      }
   }

   if(lastFlow) {
      flowSpec->SocketDescriptor         = lastFlow->SocketDescriptor;
      flowSpec->OriginalSocketDescriptor = false;
      flowSpec->StreamID                 = lastFlow->StreamID + 1;
      flowSpec->print(cout);
      cout << "      - Connection okay; sd=" << flowSpec->SocketDescriptor << endl;
   }
   else {
      flowSpec->OriginalSocketDescriptor = true;
      flowSpec->StreamID                 = 0;
      flowSpec->SocketDescriptor         = -1;
      switch(flowSpec->Protocol) {
         case IPPROTO_SCTP:
            flowSpec->SocketDescriptor = ext_socket(remoteAddress->sa_family, SOCK_STREAM, IPPROTO_SCTP);
           break;
         case IPPROTO_TCP:
            flowSpec->SocketDescriptor = ext_socket(remoteAddress->sa_family, SOCK_STREAM, IPPROTO_TCP);
           break;
         case IPPROTO_UDP:
            flowSpec->SocketDescriptor = ext_socket(remoteAddress->sa_family, SOCK_DGRAM, IPPROTO_UDP);
           break;
#ifdef IPPROTO_DCCP
         case IPPROTO_DCCP:
            flowSpec->SocketDescriptor = ext_socket(remoteAddress->sa_family, SOCK_DCCP, IPPROTO_DCCP);
           break;
#endif
      }
      flowSpec->print(cout);

      cout << "      - Connecting " << getProtocolName(flowSpec->Protocol) << " socket to ";
      printAddress(cout, remoteAddress, true);
      cout << " ... ";
      cout.flush();

      if(flowSpec->SocketDescriptor < 0) {
         cerr << "ERROR: Unable to create " << getProtocolName(flowSpec->Protocol) << " socket - " << strerror(errno) << "!" << endl;
         exit(1);
      }
      if(flowSpec->Protocol == IPPROTO_SCTP) {
         sctp_event_subscribe events;
         memset((char*)&events, 0 ,sizeof(events));
         events.sctp_data_io_event = 1;
         if(ext_setsockopt(flowSpec->SocketDescriptor, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
            cerr << "ERROR: Failed to configure events on SCTP socket - " << strerror(errno) << "!" << endl;
            exit(1);
         }
      }
      if(ext_connect(flowSpec->SocketDescriptor, remoteAddress, getSocklen(remoteAddress)) < 0) {
         cerr << "ERROR: Unable to connect " << getProtocolName(flowSpec->Protocol) << " socket - " << strerror(errno) << "!" << endl;
         exit(1);
      }
      setNonBlocking(flowSpec->SocketDescriptor);
      cout << "okay; sd=" << flowSpec->SocketDescriptor << endl;
      gMessageReader.registerSocket(flowSpec->Protocol, flowSpec->SocketDescriptor);
   }

   return(flowSpec);
}



// ###### Main loop #########################################################
bool mainLoop(const bool activeMode, const unsigned long long stopAt)
{
   int                tcpConnectionIDs[gConnectedSocketsSet.size()];
   pollfd             fds[5 + gFlowSet.size() + gConnectedSocketsSet.size()];
   int                n                     = 0;
   int                controlID             = -1;
   int                tcpID                 = -1;
   int                udpID                 = -1;
   int                sctpID                = -1;
   int                dccpID                = -1;
   unsigned long long nextStatusChangeEvent = (1ULL << 63);
   unsigned long long nextTransmissionEvent = (1ULL << 63);
   unsigned long long now                   = getMicroTime();


   // ====== Get parameters for poll() ======================================
   for(set<int>::iterator iterator = gConnectedSocketsSet.begin();iterator != gConnectedSocketsSet.end();iterator++) {
      fds[n].fd      = *iterator;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      n++;
   }
   if(gControlSocket >= 0) {
      fds[n].fd      = gControlSocket;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      controlID      = n;
      n++;
   }
   if(gTCPSocket >= 0) {
      fds[n].fd      = gTCPSocket;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      tcpID          = n;
      n++;
   }
   if(gUDPSocket >= 0) {
      fds[n].fd      = gUDPSocket;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      udpID          = n;
      n++;
   }
   if(gSCTPSocket >= 0) {
      fds[n].fd      = gSCTPSocket;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      sctpID         = n;
      n++;
   }
   if(gDCCPSocket >= 0) {
      fds[n].fd      = gDCCPSocket;
      fds[n].events  = POLLIN;
      fds[n].revents = 0;
      dccpID         = n;
      n++;
   }
   for(vector<FlowSpec*>::iterator iterator = gFlowSet.begin();iterator != gFlowSet.end();iterator++) {
      FlowSpec* flowSpec = *iterator;
      flowSpec->Index = -1;   // No index reference yet.
      if(flowSpec->SocketDescriptor >= 0) {
         if(flowSpec->OriginalSocketDescriptor) {
            assert(n < sizeof(fds) / sizeof(pollfd));
            fds[n].fd     = flowSpec->SocketDescriptor;
            fds[n].events = POLLIN;
            if( (flowSpec->OutboundFrameSize > 0.0) && (flowSpec->OutboundFrameRate <= 0.0000001) ) {
               fds[n].events |= POLLOUT;
            }
            fds[n].revents = POLLIN;
            flowSpec->Index = n++;   // Set index reference.
         }

         flowSpec->scheduleNextStatusChangeEvent(now);
         if(flowSpec->NextStatusChangeEvent < nextStatusChangeEvent) {
            nextStatusChangeEvent = flowSpec->NextStatusChangeEvent;
         }

         if( (flowSpec->Status == FlowSpec::On) &&
             (flowSpec->OutboundFrameSize > 0.0) && (flowSpec->OutboundFrameRate > 0.0000001) ) {
            flowSpec->scheduleNextTransmissionEvent();
            if(flowSpec->NextTransmissionEvent < nextTransmissionEvent) {
               nextTransmissionEvent = flowSpec->NextTransmissionEvent;
            }
         }
      }
   }


   // ====== Use poll() to wait for events ==================================
   const long long nextEvent = (long long)std::min(std::min(nextStatusChangeEvent, nextTransmissionEvent),
                                                   std::min(stopAt, gStatisticsWriter.getNextEvent()));
   const int timeout         = (int) (std::max(0LL, nextEvent - (long long)now) / 1000LL);

   // printf("to=%d   txNext=%lld\n", timeout, nextEvent - (long long)now);
   const int result = ext_poll((pollfd*)&fds, n, timeout);
   now = getMicroTime();   // Get current time.

   // ====== Handle socket events ===========================================
   if(result >= 0) {

      // ====== Incoming control message ====================================
      if( (controlID >= 0) && (fds[controlID].revents & POLLIN) ) {
         const bool controlOkay = handleControlMessage(&gMessageReader, gFlowSet, gControlSocket);
         if((!controlOkay) && (activeMode)) {
            return(false);
         }
      }

      // ====== Incoming data message =======================================
      if( (sctpID >= 0) && (fds[sctpID].revents & POLLIN) ) {
         handleDataMessage(activeMode, &gMessageReader, &gStatisticsWriter, gFlowSet, now, gSCTPSocket, IPPROTO_SCTP, gControlSocket);
      }
      if( (udpID >= 0) && (fds[udpID].revents & POLLIN) ) {
         handleDataMessage(activeMode, &gMessageReader, &gStatisticsWriter, gFlowSet, now, gUDPSocket, IPPROTO_UDP, gControlSocket);
      }
      bool gConnectedSocketsSetUpdated = false;
      if( (tcpID >= 0) && (fds[tcpID].revents & POLLIN) ) {
         const int newSD = ext_accept(gTCPSocket, NULL, 0);
         if(newSD >= 0) {
            gMessageReader.registerSocket(IPPROTO_TCP, newSD);
            gConnectedSocketsSet.insert(newSD);
            gConnectedSocketsSetUpdated = true;
         }
      }
#ifdef IPPROTO_DCCP
      if( (dccpID >= 0) && (fds[dccpID].revents & POLLIN) ) {
         const int newSD = ext_accept(gDCCPSocket, NULL, 0);
         if(newSD >= 0) {
            gMessageReader.registerSocket(IPPROTO_DCCP, newSD);
            gConnectedSocketsSet.insert(newSD);
            gConnectedSocketsSetUpdated = true;
         }
      }
#endif


      // ====== Incoming data on connected sockets ==========================
      if(!gConnectedSocketsSetUpdated) {
         for(int i = 0;i < gConnectedSocketsSet.size();i++) {
            if(fds[i].revents & POLLIN) {
               const ssize_t result = handleDataMessage(activeMode, &gMessageReader, &gStatisticsWriter, gFlowSet,
                                                        now, fds[i].fd, IPPROTO_TCP, gControlSocket);
               if( (result < 0) && (result != MRRM_PARTIAL_READ) ) {
                  gMessageReader.deregisterSocket(fds[i].fd);
                  gConnectedSocketsSet.erase(fds[i].fd);
                  ext_close(fds[i].fd);
               }
            }
         }
      }


      // ====== Outgoing flow events ========================================
      for(vector<FlowSpec*>::iterator iterator = gFlowSet.begin();iterator != gFlowSet.end();iterator++) {
         FlowSpec* flowSpec = *iterator;
         if(flowSpec->SocketDescriptor >= 0) {
            // ====== Incoming data =========================================
            if( (flowSpec->Index >= 0) && (fds[flowSpec->Index].revents & POLLIN) ) {
               handleDataMessage(activeMode, &gMessageReader, &gStatisticsWriter, gFlowSet, now, fds[flowSpec->Index].fd, flowSpec->Protocol, gControlSocket);
            }

            // ====== Status change =========================================
            if(flowSpec->NextStatusChangeEvent <= now) {
               flowSpec->statusChangeEvent(now);
            }

            // ====== Send outgoing data ====================================
            if(flowSpec->Status == FlowSpec::On) {
               // ====== Outgoing data (saturated sender) ===================
               if( (flowSpec->OutboundFrameSize > 0.0) && (flowSpec->OutboundFrameRate <= 0.0000001) ) {
                  if(fds[flowSpec->Index].revents & POLLOUT) {
                     transmitFrame(&gStatisticsWriter, flowSpec, now, gMaxMsgSize);
                  }
               }

               // ====== Outgoing data (non-saturated sender) ===============
               if( (flowSpec->OutboundFrameSize >= 1.0) && (flowSpec->OutboundFrameRate > 0.0000001) ) {
                  const unsigned long long lastEvent = flowSpec->LastTransmission;
                  if(flowSpec->NextTransmissionEvent <= now) {
                     do {
                        transmitFrame(&gStatisticsWriter, flowSpec, now, gMaxMsgSize);
                        if(now - lastEvent > 1000000) {
                           // Time gap of more than 1s -> do not try to correct
                           break;
                        }
                        flowSpec->scheduleNextTransmissionEvent();
                     } while(flowSpec->NextTransmissionEvent <= now);
                  }
               }
            }
         }
      }


      // ====== Stop-time reached ===========================================
      if(now >= stopAt) {
         gStopTimeReached = true;
      }
   }

   // ====== Handle statistics timer ========================================
   if(gStatisticsWriter.getNextEvent() <= now) {
      gStatisticsWriter.writeVectorStatistics(now, gFlowSet);
   }

   return(true);
}



// ###### Passive Mode ######################################################
void passiveMode(int argc, char** argv, const uint16_t localPort)
{
   // ====== Initialize control socket ======================================
   gControlSocket = createAndBindSocket(SOCK_SEQPACKET, IPPROTO_SCTP, localPort + 1, true);   // Leave it blocking!
   if(gControlSocket < 0) {
      cerr << "ERROR: Failed to create and bind SCTP socket for control port - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   sctp_event_subscribe events;
   memset((char*)&events, 0 ,sizeof(events));
   events.sctp_data_io_event     = 1;
   events.sctp_association_event = 1;
   if(ext_setsockopt(gControlSocket, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
      cerr << "ERROR: Failed to configure events on control socket - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   gMessageReader.registerSocket(IPPROTO_SCTP, gControlSocket);

   // ====== Initialize data socket for each protocol =======================
   gTCPSocket = createAndBindSocket(SOCK_STREAM, IPPROTO_TCP, localPort);
   if(gTCPSocket < 0) {
      cerr << "ERROR: Failed to create and bind TCP socket - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   gUDPSocket = createAndBindSocket(SOCK_DGRAM, IPPROTO_UDP, localPort);
   if(gUDPSocket < 0) {
      cerr << "ERROR: Failed to create and bind UDP socket - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   gMessageReader.registerSocket(IPPROTO_UDP, gUDPSocket);
#ifdef IPPROTO_DCCP
   gDCCPSocket = createAndBindSocket(SOCK_DCCP, IPPROTO_DCCP, localPort);
   if(gDCCPSocket < 0) {
      cerr << "NOTE: Your kernel does not provide DCCP support." << endl;
   }
#endif
   gSCTPSocket = createAndBindSocket(SOCK_SEQPACKET, IPPROTO_SCTP, localPort);
   if(gSCTPSocket < 0) {
      cerr << "ERROR: Failed to create and bind SCTP socket - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   memset((char*)&events, 0 ,sizeof(events));
   events.sctp_data_io_event = 1;
   if(ext_setsockopt(gSCTPSocket, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
      cerr << "ERROR: Failed to configure events on SCTP socket - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   gMessageReader.registerSocket(IPPROTO_SCTP, gSCTPSocket);


   // ====== Set options ====================================================
   for(int i = 2;i < argc;i++) {
      if(!handleGlobalParameter(argv[i])) {
         std::cerr << "Invalid argument: " << argv[i] << "!" << std::endl;
         exit(1);
      }
   }
   printGlobalParameters();

   // ====== Print status ===================================================
   cout << "Passive Mode: Accepting TCP/UDP/SCTP" << ((gDCCPSocket > 0) ? "/DCCP" : "")
        << " connections on port " << localPort << endl << endl;


   // ====== Main loop ======================================================
   installBreakDetector();
   const unsigned long long stopAt = (gRuntime > 0) ? (getMicroTime() + (unsigned long long)rint(gRuntime * 1000000.0)) : ~0ULL;
   while( (!breakDetected()) && (!gStopTimeReached) ) {
      mainLoop(false, stopAt);
   }


   // ====== Print status ===================================================
   FlowSpec::printFlows(cout, gFlowSet, true);


   // ====== Clean up =======================================================
   gMessageReader.deregisterSocket(gControlSocket);
   ext_close(gControlSocket);
   ext_close(gTCPSocket);
   gMessageReader.deregisterSocket(gUDPSocket);
   ext_close(gUDPSocket);
   gMessageReader.deregisterSocket(gSCTPSocket);
   ext_close(gSCTPSocket);
   if(gDCCPSocket >= 0) {
      ext_close(gDCCPSocket);
   }
}



// ###### Active Mode #######################################################
void activeMode(int argc, char** argv)
{
   // ====== Initialize remote and control addresses ========================
   sockaddr_union remoteAddress;
   if(string2address(argv[1], &remoteAddress) == false) {
      cerr << "ERROR: Invalid remote address " << argv[1] << "!" << endl;
      exit(1);
   }
   if(getPort(&remoteAddress.sa) == 0) {
      setPort(&remoteAddress.sa, 9000);
   }
   sockaddr_union controlAddress = remoteAddress;
   setPort(&controlAddress.sa, getPort(&remoteAddress.sa) + 1);


   // ====== Initialize IDs and print status ================================
   uint32_t flows         = 0;
   uint64_t measurementID = getMicroTime();
   char     mIDString[32];
   snprintf((char*)&mIDString, sizeof(mIDString), "%lx", measurementID);
   cout << "Active Mode:" << endl
        << "   - Measurement ID  = " << mIDString << endl
        << "   - Remote Address  = "; printAddress(cout, &remoteAddress.sa, true);  cout << endl
        << "   - Control Address = "; printAddress(cout, &controlAddress.sa, true); cout << " - connecting ... ";
   cout.flush();


   // ====== Initialize control socket ======================================
   gControlSocket = ext_socket(controlAddress.sa.sa_family, SOCK_STREAM, IPPROTO_SCTP);
   if(gControlSocket < 0) {
      cerr << "ERROR: Failed to create SCTP socket for control port - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   if(ext_connect(gControlSocket, &controlAddress.sa, getSocklen(&controlAddress.sa)) < 0) {
      cerr << "ERROR: Unable to establish control association - " << strerror(errno) << "!" << endl;
      exit(1);
   }
   cout << "okay; sd=" << gControlSocket << endl << endl;
   gMessageReader.registerSocket(IPPROTO_SCTP, gControlSocket);


   // ====== Initialize flows ===============================================
   uint8_t   protocol = 0;
   FlowSpec* lastFlow      = NULL;
   for(int i = 2;i < argc;i++) {
      if(argv[i][0] == '-') {
         lastFlow = NULL;
         if(handleGlobalParameter(argv[i])) {
         }
         else if(strcmp(argv[i], "-tcp") == 0) {
            protocol = IPPROTO_TCP;
         }
         else if(strcmp(argv[i], "-udp") == 0) {
            protocol = IPPROTO_UDP;
         }
         else if(strcmp(argv[i], "-sctp") == 0) {
            protocol = IPPROTO_SCTP;
         }
         else if(strcmp(argv[i], "-dccp") == 0) {
#ifdef IPPROTO_DCCP
            protocol = IPPROTO_DCCP;
#else
            cerr << "ERROR: DCCP support is not compiled in!" << endl;
            exit(1);
#endif
         }
         else if(strncmp(argv[i], "-vector=", 8) == 0) {
            if(gStatisticsWriter.VectorName != NULL) {
               cerr << "ERROR: Vector file name has been specified twice!" << endl;
               exit(1);
            }
            gStatisticsWriter.VectorName = (const char*)&argv[i][8];
            if(gStatisticsWriter.openOutputFile(gStatisticsWriter.VectorName, &gStatisticsWriter.VectorFile, &gStatisticsWriter.VectorBZFile) == false) {
               exit(1);
            }
         }
         else if(strncmp(argv[i], "-scalar=", 8) == 0) {
            if(gStatisticsWriter.ScalarName != NULL) {
               cerr << "ERROR: Scalar file name has been specified twice!" << endl;
               exit(1);
            }
            gStatisticsWriter.ScalarName = (const char*)&argv[i][8];
            if(gStatisticsWriter.openOutputFile(gStatisticsWriter.ScalarName, &gStatisticsWriter.ScalarFile, &gStatisticsWriter.ScalarBZFile) == false) {
               exit(1);
            }
         }
      }
      else {
         if(protocol == 0) {
            cerr << "ERROR: Protocol specification needed before flow specification " << argv[i] << "!" << endl;
            exit(1);
         }

         lastFlow = createLocalFlow(argv[i], protocol, lastFlow,
                                    measurementID, &flows,
                                    &remoteAddress.sa, &controlAddress.sa);
         gFlowSet.push_back(lastFlow);

         cout << "      - Registering flow at remote node ... ";
         cout.flush();
         if(!addFlowToRemoteNode(gControlSocket, lastFlow)) {
            cerr << "ERROR: Failed to add flow to remote node!" << endl;
            exit(1);
         }
         cout << "okay" << endl;

         if(protocol != IPPROTO_SCTP) {
            lastFlow = NULL;
            protocol = 0;
         }
      }
   }
   cout << endl;

   printGlobalParameters();


   // ====== Start measurement ==============================================
   const unsigned long long now = getMicroTime();
   if(!startMeasurement(gControlSocket, measurementID)) {
      std::cerr << "ERROR: Failed to start measurement!" << std::endl;
      exit(1);
   }
   for(vector<FlowSpec*>::iterator iterator = gFlowSet.begin();iterator != gFlowSet.end(); iterator++) {
      FlowSpec* flowSpec = *iterator;
      flowSpec->BaseTime = now;
      flowSpec->Status   = (flowSpec->OnOffEvents.size() > 0) ? FlowSpec::Off : FlowSpec::On;
   }


   // ====== Main loop ======================================================
   const unsigned long long stopAt  = (gRuntime > 0) ? (getMicroTime() + (unsigned long long)rint(gRuntime * 1000000.0)) : ~0ULL;
   bool                     aborted = false;
   installBreakDetector();
   while( (!breakDetected()) && (!gStopTimeReached) ) {
      if(!mainLoop(true, stopAt)) {
         cout << endl << "*** Aborted ***" << endl;
         aborted = true;
         break;
      }
   }


   // ====== Stop measurement ===============================================
   if(!stopMeasurement(gControlSocket, measurementID)) {
      std::cerr << "ERROR: Failed to stop measurement!" << std::endl;
      exit(1);
   }


   // ====== Write statistics ===============================================
   if(!aborted) {
      gStatisticsWriter.writeScalarStatistics(getMicroTime(), gFlowSet);   // Write scalar statistics first!
   }
   gStatisticsWriter.closeOutputFile(gStatisticsWriter.VectorName, &gStatisticsWriter.VectorFile, &gStatisticsWriter.VectorBZFile);
   gStatisticsWriter.closeOutputFile(gStatisticsWriter.ScalarName, &gStatisticsWriter.ScalarFile, &gStatisticsWriter.ScalarBZFile);
   if(!aborted) {
      FlowSpec::printFlows(cout, gFlowSet, true);
   }


   // ====== Clean up =======================================================
   cout << "Shutdown:" << endl;
   vector<FlowSpec*>::iterator iterator = gFlowSet.begin();
   while(iterator != gFlowSet.end()) {
      const FlowSpec* flowSpec = *iterator;
      cout << "   o Flow ID #" << flowSpec->FlowID << " ... ";
      cout.flush();
      if(flowSpec->OriginalSocketDescriptor) {
         gMessageReader.deregisterSocket(flowSpec->SocketDescriptor);
      }
      removeFlowFromRemoteNode(gControlSocket, flowSpec);
      delete flowSpec;
      gFlowSet.erase(iterator);
      cout << "okay" << endl;
      iterator = gFlowSet.begin();
   }
   cout << endl;
   gMessageReader.deregisterSocket(gControlSocket);
   ext_close(gControlSocket);
}



// ###### Main program ######################################################
int main(int argc, char** argv)
{
   if(argc < 2) {
      cerr << "Usage: " << argv[0] << " [Port|Remote Endpoint] {-tcp|-udp|-sctp|-dccp} {flow spec} ..." << endl;
      exit(1);
   }

   cout << "Network Performance Meter - Version 1.0" << endl
        << "---------------------------------------" << endl << endl;

   const uint16_t localPort = atol(argv[1]);
   if( (localPort >= 1024) && (localPort < 65535) ) {
      passiveMode(argc, argv, localPort);
   }
   else {
      activeMode(argc, argv);
   }
   cout << endl;

   return 0;
}