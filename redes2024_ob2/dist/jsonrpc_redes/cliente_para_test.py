import socket
import json

class JSONRPCError(Exception):
	def __init__(self, code, message):
		self.code = code
		self.message = message
		super().__init__(f"Error {code}: {message}")

class ClientTest:
	def __init__(self, address, port):
		self.address = address
		self.port = port
		self.connected = False
		self.id = 1
		# Establecemos la conexion
		self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		self.socket.connect((address, port))
		self.new_message = ''
		# print(f"Connecting to {address}:{port}...")
		self.connected = True

	def __getattr__(self, name):
		# Trying to call a remote procedure
		def method(*args, notify=False, **kwargs):
			if not self.connected:
				raise ConnectionError("Not connected to the server.")
			# Prepare the request to send to the server
			# print(f"Calling remote procedure '{name}' with args: {args}, notify: {notify}")
			args_list = list(args)
			
			if name == 'wrong_JSON_version':
				json_data = {
					"jsonrpc": "1.0",
					"method": name,
					"params": args or kwargs,
					"id": self.id if not notify else None
				}
			elif name == 'no_method':
				json_data = {
					"jsonrpc": "2.0",
					"params": args or kwargs,
					"id": self.id if not notify else None
				}
			elif name == 'method_is_number':
				json_data = {
					"jsonrpc": "2.0",
					"method": 25,
					"params": args or kwargs,
					"id": self.id if not notify else None
				}
			elif name == 'incomplete_JSON':
				json_data = {
					"jsonrpc": "2.0",
					"method": "incompl"
				}
			else:
				json_data = {
					"jsonrpc": "2.0",
					"method": name,
					"params": args or kwargs,
					"id": self.id if not notify else None
				} 
			self.id += 1

			# print(json_data)
			msg = json.dumps(json_data)
			if name == 'dont_close_JSON':
				msg = '{' + msg
			elif name == 'extra_close_JSON':
				msg = msg + '}'
			elif name == 'empty_JSON':
				msg = ''
			elif name == 'incomplete_JSON':
				msg = msg[:-2]
			self.sendMessage(msg)  

			if notify:
				print(f"Notification sent for {name}")
				return None
			else:
				# Receive the response and attempt to parse it
				response, self.new_message = self.receiveMessage(self.new_message)
				response_json = json.loads(response)
				
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
		
	def sendMessage(self, msg):
		self.socket.sendall(bytes(msg, "utf-8"))

	def receiveMessage(self, new_message):
		full_msg = new_message
		bracket_count = 0
		prev_char = ''

		while True:
			msg = self.socket.recv(16)
			if not msg:
				break

			msg_str = msg.decode("utf-8")
			pos = 0

			# Update bracket count and determine where to stop
			for c in msg_str:
				if c == '}' and prev_char != '\\':
					bracket_count -= 1
				elif c == '{' and prev_char != '\\':
					bracket_count += 1
				
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

def connectTest(address, port):
	# This public function establishes the connection and returns a Connection object
	return ClientTest(address, port)
