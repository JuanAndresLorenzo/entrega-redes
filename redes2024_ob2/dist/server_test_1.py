from jsonrpc_redes.servidor import Server
import threading
import time
import sys

def server1():
    # Se inicia un servidor en el puerto 8081 y se añaden tres métodos
    
    host, port = '', 8081
    
    def echo_repeat(message, repeatTimes):
        ret = ''
        for _ in range(repeatTimes):
            ret += message
        return ret
        
    def summation(*args):
        return sum(args)

    def echo_concat(msg1, msg2, msg3, msg4="parametroVacio"):
        return msg1 + msg2 + msg3 + msg4
    
    def HolaMundo():
        return 'HolaMundo!...'
    
    def echo_dos_args(arg1, arg2):
        return (arg1, arg2)
    
    server = Server((host, port))
    server.add_method(echo_repeat)
    server.add_method(summation, 'sum')
    server.add_method(echo_concat)
    server.add_method(HolaMundo)
    server.add_method(echo_dos_args)
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
    server1()