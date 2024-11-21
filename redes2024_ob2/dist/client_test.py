from jsonrpc_redes.cliente import connect
from cliente_para_test import connectTest

def test_client():
    # Tests para el cliente

    print('=============================')
    print('Iniciando pruebas de casos sin errores para servidor 1.')

    connS1 = connect('200.0.0.10', 8081)

    arg1 , arg2 = connS1.echo_dos_args("hola", "mundo")
    assert arg1 == 'hola' and arg2 == 'mundo'
    print('Test metodo con doble retorno completado.')

    result = connS1.HolaMundo()
    assert result == 'HolaMundo!...'
    print('Test HolaMundo (llamado a metodo sin parametros) completado.')

    result = connS1.HolaMundo(notify=True)
    assert result == None
    print('Test HolaMundo (llamado a metodo sin parametros con notificar) completado.')

    value = 'Testing!'
    result = connS1.echo_repeat(value, 2)
    assert result == value + value
    print('Test echo_repeat completado.')

    value = 'Testing!'
    result = connS1.echo_repeat(value, 2, notify=True)
    assert result == None
    print('Test echo_repeat (llamado a metodo con parámetros con notificación) completado.')
    
    result = connS1.sum(1, 2, 3, 4, 5)
    assert result == 15
    print('Test de suma completado.')

    result = connS1.echo_concat('a', 'b', 'c', 'd')
    assert result == 'abcd'
    print('Test de echo_concat completado')
    
    result = connS1.echo_repeat(message='No response!', repeatTimes=1, notify=True)
    assert result == None
    print('Test de notificación completado.')

    result = connS1.echo_concat(msg1='a', msg2='b', msg3='c', msg4='d')
    assert result == 'abcd'
    print('Test de múltiples parámetros con nombres completado')

    result = connS1.echo_concat('a', 'b', 'c')
    assert result == 'abcparametroVacio'
    print('Test de múltiples parámetros con parametro opcional vacio completado')

    result = connS1.echo_concat(msg1='a', msg2='b', msg3='c')
    assert result == 'abcparametroVacio'
    print('Test de múltiples parámetros con nombre y parametro opcional vacio completado')

    result = connS1.sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    assert result == 55
    print('Segundo test de suma con 10 parámetros completado')

    print('=============================')
    print('Pruebas de casos sin errores en servidor 1 completadas.')
    print('=============================')
    print('Iniciando pruebas de casos sin errores para servidor 2.')

    connS2 = connect('200.100.0.15', 8082)
    result = connS2.divide(8, 2)
    assert result == 4
    print('Test divide completado.')
    
    result = connS2.multiply(1, 2, 3, 4, 5)
    assert result == 120
    print('Test de multiplicacion completado.')

    result = connS2.concat_reverse('Hola', 'Mundo!')
    assert result == 'Hola!odnuM'
    print('Test de concat_reverse completado')
    
    result = connS2.concat_reverse(msg1='No response!', msg_reverse="asdasd", notify=True)
    assert result == None
    print('Test de notificación completado.')

    result = connS2.concat_reverse(msg1='Hola!', msg_reversed="Mundo!")
    assert result == 'Hola!!odnuM'
    print('Test de múltiples parámetros con nombres completado')

    print('=============================')
    print('Pruebas de casos sin errores completadas.')
    print('=============================')
    print('Iniciando pruebas de casos con errores.')

    
    try:
        connS1.echo_concat()
    except Exception as e:
        print('Llamada incorrecta sin parámetros. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')
        
    try:
        connS1.multiply(5, 6)
    except Exception as e:
        print('Llamada a método inexistente. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS2.concat_reverse('a', 'b', 'c', 4)
    except Exception as e:
        print('Llamada incorrecta genera excepción interna del servidor.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS2.concat_reverse('a', 'b', 'c')
    except Exception as e:
        print('Llamada incorrecta menos parametros de los esperados.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')    

    try:
        connS2.concat_reverse('a')
    except Exception as e:
        print('Llamada incorrecta mas parametros de los esperados.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS2.divide(1, arg2=4)
    except Exception as e:
        print('Llamada incorrecta genera excepción en el cliente.')
        print(e)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS2.divide(4, 0)
    except Exception as e:
        print('Llamada incorrecta genera excepción interna del servidor.')
        print(e)
    else:
        print('ERROR: No lanzó excepción.')
    
    try:
        connS2.concat_reverse(msg1='a', msg_revers='b')
    except Exception as e:
        print('Llamada incorrecta genera excepcion por nombre equivocado de parámetros.') 
        print(e)
    else:
        print('ERROR: No lanzó excepción.')
    
    print('=============================')
    print("Pruebas de casos con errores completadas.")
    print('=============================')
    print('Iniciando pruebas de casos con JSON incorrectos')

    connS3 = connectTest('200.100.0.15', 8081)

    try:
        connS3.wrong_JSON_version()
    except Exception as e:
        print('Llamada incorrecta con JSON incorrecto. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS3.no_method()
    except Exception as e:
        print('Llamada incorrecta con JSON sin metodo. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')
    
    try:
        connS3.method_is_number()
    except Exception as e:
        print('Llamada incorrecta con metodo no string. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')

    try:
        connS3.incomplete_JSON()
    except Exception as e:
        print('Llamada incorrecta con JSON incompleto. Genera excepción necesaria.')
        print(e.code, e.message)
    else:
        print('ERROR: No lanzó excepción.')
    
if __name__ == "__main__":
    test_client()