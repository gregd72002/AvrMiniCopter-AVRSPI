# AvrMiniCopter-AVRSPI

AVRSPI provides a UNIX SOCKET & UDP service for clients to connect to in order to communicate with the controller (AvrMiniCopter-Arduino). It is mainly used to query current status, issue target position request, forward altitude reading etc.

By default AVRSPI binds to UDP port 1030 and UNIX SOCKET /dev/avrspi

#### Message structure
Each message is of a fixed length - 4 bytes:
- control (uint8_t)
- type (uint8_t)
- value (int16_t)

Control (for incoming messages):
- currently only used when using UDP
- 0: do not remember the client, just process the incoming message
- 1: remember the client and process the incoming message
- 2: remember the client and ping back

Control (for outgoing messages):
- 0: a regular message
- 1: disconnect request

Type:
- messages with type 0 will be handled by AVRSPI
  Values:
   - 0: dummy message
   - 1: reset backend and initiate autoconfig
   - 2: reset backend but do not initiate autoconfig
   - 3: get number of CRC errors for incoming SPI transfers
   - 4: save flight log
- all other types will be forwarded to the backend; see the backend documentation

#### MESSAGES
For full list of available message see https://github.com/rpicopter/AvrMiniCopter-WWW/blob/master/www/debug_help.txt

#### UNIX SOCKET
When connecting over UNIX SOCKET the client needs to send the required connection type as the first byte.
This is:
- 0: normal bi-directional connection 
- 1: one-way connection - in this case AVRSPI will not be sending any information back to the client (useful for services like barometer reader and similar)



#### TODO
- remove flight log and configurator 

