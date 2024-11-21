from jsonrpc_redes.servidor import Server
import threading
import time
import sys

def server2():
	# Se inicia un servidor en el puerto 8082 y se añaden tres métodos
	
	host, port = '', 8082
	
	def divide(arg1, arg2): 
		return arg1 / arg2
		
	def multiply(*args):
		ret = 1
		for item in args:
			ret *= item
		return ret

	def concat_reverse(msg1, msg_reversed):
		return msg1 + msg_reversed[::-1]
		
	server = Server((host, port))
	server.add_method(divide)
	server.add_method(multiply)
	server.add_method(concat_reverse)
	server_thread = threading.Thread(target=server.serve)
	server_thread.daemon = True
	server_thread.start()
	
	print ("Servidor ejecutando: %s:%s" % (host, port))
	
	try:
		while True:
			time.sleep(0.5)
	except KeyboardInterrupt:
		server.shutdown()
		print('Terminado.')
		sys.exit()
	
if __name__ == "__main__":
	server2()