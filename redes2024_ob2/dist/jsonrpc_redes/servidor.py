import socket
import json
import inspect
import threading
import time

class Server:
    def __init__(self, address_port):
        self.address, self.port = address_port
        self.new_message = ''
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # Allow reusing the address
        self.methods = {}
        self.timeout = 1

    def add_method(self, func, method_name=None):
        # Allow method_name to be passed, otherwise use the function's own name
        if method_name is None:
            method_name = func.__name__
        self.methods[method_name] = func

    def serve(self):
        self.server.bind((self.address, self.port))
        print(f"Server running on {self.address}:{self.port}")
        self.server.listen(5)

        while True:
            clientsocket, address = self.server.accept()
            print(f"Connection from {address} has been established.")
            
            # Start a new thread to handle each client connection
            client_thread = threading.Thread(target=self.handle_client, args=(clientsocket, address))
            client_thread.start()

    def handle_client(self, clientsocket, address):
        try:
            while True:
                full_msg = self.new_message
                bracket_count = 0
                is_quoted = False # Track whether we are inside quotes
                prev_char = ''
                clientsocket.settimeout(self.timeout)

                while True:
                    # Receive the next part of the message
                    try:
                        msg = clientsocket.recv(16)
                    except socket.timeout:  
                        break

                    msg_str = msg.decode("utf-8")
                    pos = 0 # no estoy seguro de si esta haciendo algo

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
                        self.new_message = msg_str[pos:]
                        break
                    else:
                        self.new_message = ""

                    # Check if the message is complete
                    if bracket_count == 0 and len(full_msg) > 0:
                        break

                try:
                    request_json = json.loads(full_msg)
                    print("received request: ", request_json)
                except json.JSONDecodeError:
                    print("Received incomplete or invalid JSON")

                    error_response = {
                        "jsonrpc": "2.0",
                        "error": {
                            "code": -32700,
                            "message": "Parse error"
                        },
                        "id": None
                    }

                    clientsocket.sendall(bytes(json.dumps(error_response), 'utf-8'))
                    clientsocket.close()
                    continue

                response_json = self.handle_request(request_json)
                if response_json is not None:
                    response_msg = json.dumps(response_json)
                    clientsocket.sendall(bytes(response_msg, 'utf-8'))

        except Exception as e:
            print(f"Error handling connection from {address}: {e}")
        finally:
            clientsocket.close()

    def handle_request(self, request_json):
        try:
            if not request_json.get("jsonrpc") == "2.0":
                raise ValueError({"code": -32600, "message": "Invalid Request", "data": "Invalid JSON-RPC version"})

            method_name = request_json.get("method")
            if not method_name:
                raise ValueError({"code": -32600, "message": "Invalid Request", "data": "The method name is missing."})
            if not isinstance(method_name, str):
                raise ValueError({"code": -32600, "message": "Invalid Request", "data": "The method name must be a string."})
            params = request_json.get("params", [])

            # Check if method exists in the registered methods dictionary
            if method_name in self.methods:
                method = self.methods[method_name]

                try:
                    if request_json.get("id") is None:
                        result = None
                    else:
                        if isinstance(params, list):
                            # Get the method signature
                            signature = inspect.signature(method)
                            parameters = signature.parameters

                            # Required parameters (those without defaults and aren't *args or **kwargs)
                            required_params = [p for p in parameters.values() if p.default == inspect.Parameter.empty and p.kind not in (inspect.Parameter.VAR_POSITIONAL, inspect.Parameter.VAR_KEYWORD)]

                            # Ensure the number of positional arguments matches the required positional parameters
                            if len(params) < len(required_params):
                                raise ValueError({
                                    "code": -32602,
                                    "message": "Invalid params",
                                    "data": "Missing required positional parameters."
                                })

                            # Check if too many positional arguments are provided
                            if len(params) > len(parameters) and not any(p.kind == inspect.Parameter.VAR_POSITIONAL for p in parameters.values()):
                                raise ValueError({
                                    "code": -32602,
                                    "message": "Invalid params",
                                    "data": "Too many positional parameters provided."
                                })

                            # If all checks are good, execute the method with positional arguments
                            result = method(*params)

                        elif isinstance(params, dict):
                            # Get the method signature
                            signature = inspect.signature(method)
                            parameters = signature.parameters

                            # Required parameters (those without defaults and aren't *args or **kwargs)
                            required_params = [p for p in parameters.values() if p.default == inspect.Parameter.empty and p.kind not in (inspect.Parameter.VAR_POSITIONAL, inspect.Parameter.VAR_KEYWORD)]

                            # Ensure all required keyword arguments are provided
                            missing_params = [p for p in required_params if p.name not in params]
                            if missing_params:
                                raise ValueError({
                                    "code": -32602,
                                    "message": "Invalid params",
                                    "data": f"Missing required parameters: {', '.join(p.name for p in missing_params)}"
                                })

                            # Ensure no extra keyword arguments are provided if the method doesn't accept **kwargs
                            if not any(p.kind == inspect.Parameter.VAR_KEYWORD for p in parameters.values()):
                                unexpected_params = [key for key in params if key not in parameters]
                                if unexpected_params:
                                    raise ValueError({
                                        "code": -32602,
                                        "message": "Invalid params",
                                        "data": f"Unexpected parameters: {', '.join(unexpected_params)}"
                                    })

                            # If all checks are good, execute the method with keyword arguments
                            result = method(**params)

                        else:
                            raise ValueError({
                                "code": -32602,
                                "message": "Invalid request",
                                "data": "Params must be a list or a dictionary."
                            })
                except ValueError as ve:
                    return {"jsonrpc": "2.0", "error": ve.args[0], "id": request_json["id"]}
                except Exception as e:
                    raise AttributeError({"code": -32603, "message": "Internal error", "data": f"There was an error in the server: {e}"})

                if result is None:
                    return None
                else:
                    return {"jsonrpc": "2.0", "result": result, "id": request_json["id"]}
            else:
                raise AttributeError({"code": -32601, "message": "Method not found", "data": f"Method {method_name} not found."})

        except ValueError as ve:
            return {"jsonrpc": "2.0", "error": ve.args[0], "id": request_json["id"]}
        except AttributeError as ae:
            return {"jsonrpc": "2.0", "error": ae.args[0], "id": request_json["id"]}
        except Exception as e:
            return {"jsonrpc": "2.0", "error": {"code": -32603, "message": "Internal error", "data": str(e)}, "id": request_json["id"]}
