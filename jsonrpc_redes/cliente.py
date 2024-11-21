import socket
import json
import uuid

class JSONRPCError(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message
        super().__init__(f"Error {code}: {message}")

class Client:
    def __init__(self, address, port):
        self.address = address
        self.port = port
        self.connected = False
        # Establish connection
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.connect((address, port))
        self.new_message = ''
        # print(f"Connecting to {address}:{port}...")
        self.connected = True
        self.timeout = 1

    def __getattr__(self, name):
        # Trying to call a remote procedure
        def method(*args, notify=False, **kwargs):
            if not self.connected:
                raise ConnectionError("Not connected to the server.")
            # Prepare the request to send to the server
            # print(f"Calling remote procedure '{name}' with args: {args}, notify: {notify}")
            args_list = list(args)
            
            json_data = {
                "jsonrpc": "2.0",
                "method": name,
                "params": args or kwargs
            } 

            if not notify:
                json_data["id"] = uuid.uuid4().int

            # print(json_data)
            msg = json.dumps(json_data)
            self.sendMessage(msg)  

            if notify:
                print(f"Notification sent for {name}")
                return None
            else:
                # Receive the response and attempt to parse it
                response, self.new_message = self.receiveMessage(self.new_message)
                try:
                    response_json = json.loads(response)
                except json.JSONDecodeError:
                    raise Exception("Invalid JSON-RPC response: Failed to parse JSON.")
                
                # Check if the response contains an error (per JSON-RPC format)
                if "error" in response_json:
                    error_code = response_json["error"].get("code", "Unknown code")
                    error_message = response_json["error"].get("message", "Unknown error message")
                    raise JSONRPCError(error_code, error_message)
                
                # Check if the response contains a result and return it
                if "result" in response_json:
                    return response_json["result"]
                else:
                    raise Exception("Invalid JSON-RPC response: Missing 'result' field.")
        return method
    
    def sendMessage(self, msg):
        self.socket.sendall(bytes(msg, "utf-8"))

    def receiveMessage(self, new_message):
        full_msg = new_message
        bracket_count = 0
        is_quoted = False # Track whether we are inside quotes
        prev_char = ''
        self.socket.settimeout(self.timeout)

        while True:
            try:
                msg = self.socket.recv(16)
            except socket.timeout:
                break

            msg_str = msg.decode("utf-8")
            pos = 0

            # Update bracket count and determine where to stop
            for c in msg_str:
                if c == '}' and not is_quoted:
                    bracket_count -= 1
                elif c == '{' and not is_quoted :
                    bracket_count += 1
                elif c == '"' and prev_char != '\\': # \ is escape character, so it is not a quote.
                    is_quoted = not is_quoted

                prev_char = c
                pos += 1

                # If bracket count is balanced, stop receiving more
                if bracket_count == 0 and pos > 0:
                    break

            # Append the part of the message up to the balanced point
            full_msg += msg_str[:pos]

            # Update the remaining message if we stopped mid-way
            if pos < len(msg_str):
                new_message = msg_str[pos:]
                # print(f"Partial message received: {new_message} at position {pos}")
                break
            else:
                new_message = ""

            # Check if the message is complete
            if bracket_count == 0 and len(full_msg) > 0:
                # print("Full message received")
                break

        return full_msg, new_message


    def close(self):
        self.socket.close()

def connect(address, port):
    # This public function establishes the connection and returns a Connection object
    return Client(address, port)
