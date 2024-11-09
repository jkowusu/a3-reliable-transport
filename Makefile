# Compiler and flags
CC = g++
CFLAGS = -Wall -g

# Executable names
SENDER_BASE = wSender-base
RECEIVER_BASE = wReceiver-base
SENDER_OPT = wSender-opt
RECEIVER_OPT = wReceiver-opt

# Source files
SENDER_SRC = wSender.cpp
RECEIVER_SRC = wReceiver.cpp
SENDER_OPT_SRC = wSenderOpt.cpp
RECEIVER_OPT_SRC = wReceiverOpt.cpp

# Rules
all: $(SENDER_BASE) $(RECEIVER_BASE) $(SENDER_OPT) $(RECEIVER_OPT)

$(SENDER_BASE): $(SENDER_SRC)
	$(CC) $(CFLAGS) -o $(SENDER_BASE) $(SENDER_SRC)

$(RECEIVER_BASE): $(RECEIVER_SRC)
	$(CC) $(CFLAGS) -o $(RECEIVER_BASE) $(RECEIVER_SRC)

$(SENDER_OPT): $(SENDER_OPT_SRC)
	$(CC) $(CFLAGS) -o $(SENDER_OPT) $(SENDER_OPT_SRC)

$(RECEIVER_OPT): $(RECEIVER_OPT_SRC)
	$(CC) $(CFLAGS) -o $(RECEIVER_OPT) $(RECEIVER_OPT_SRC)

clean:
	rm -f $(SENDER_BASE) $(RECEIVER_BASE) $(SENDER_OPT) $(RECEIVER_OPT)
