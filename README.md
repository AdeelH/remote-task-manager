# Description
A commandline app that allows clients to execute programs on a remote computer. Supports multiple concurrent clients.

# Commands

## Client
```Shell
# Connect to the server.
> conn[ect] <hostname> <port>

# Close the Task Manager and the socket. Keep the client running.
> disconnect
```

## Client -> Task Manager
```Shell
# Sleep for <seconds> seconds. (Can be used to check the non-blocking behavior of the client).
> sleep <seconds>

# Start <count> instances of the program.
[run] <program-name> [<count>]

# List alive processes.
> list

# List all alive or dead processes started through the Task Manager.
> list all

# List all alive or dead processes started through the Task Manager, 
# along with their start, end, and the elapsed times.
> list details

# Kill process by pid or name
> kill [<pid> | <process-name>]

# Kill all processes
> kill [all | *]
> add [<num1> [<num2> …]]
> sub [<num1> [<num2> …]]
> mul [<num1> [<num2> …]]

# Add, subtract, multiply or divide.
> div [<num1> [<num2> …]]

# Exit the Task Manager.
> exit | ex | quit | q | disconnect
```

## Client -> Task Manager -> Server
```Shell
# Message shows up on the server terminal.
> msg <message>
```

## Server
```Shell
# List currently connected clients.
> list

# Disconnect this particular client.
> disconnect <ip>:<port>

# Disconnect all clients.
> disconnect all

# Disconnect any connected clients and exit.
> exit | ex | quit | q
```

## Server -> Task Manager -> Client
```Shell
# Message shows up each client’s terminal.
> broadcast <message>

# Send command to this client’s Task Manager and receive the output.
> cl <ip>:<port> <command>
```
