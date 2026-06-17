import asyncio
from aiocoap import *

async def enviar_datos():
    # 1. Creamos el cliente CoAP
    context = await Context.create_client_context()

    # 2. El JSON que le mandamos a tu ESP32 (Juega con estos valores)
    payload = b'{"hum_a": 35, "hum_s": 60, "level": 100}'
    
    # 3. Configuramos la peticion POST a la ruta /sensores (CAMBIA LA IP)
    uri = "coap://192.168.43.197/sensores"
    request = Message(code=POST, payload=payload, uri=uri)

    print(f"Enviando paquete CoAP a {uri}...")

    try:
        # 4. Disparamos la peticion y esperamos respuesta
        response = await context.request(request).response
        print(f"Exito. Respuesta de la ESP32: Código {response.code}")
    except Exception as e:
        print(f"Error de conexion: {e}")

if __name__ == "__main__":
    # Bucle asincrono de Python
    asyncio.run(enviar_datos())