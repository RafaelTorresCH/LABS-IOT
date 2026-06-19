import asyncio
import json
import random
from aiocoap import *

URI = "coap://192.168.43.197/sensores"  # Cambia la IP si es necesario
INTERVALO = 5  # segundos

async def enviar_datos():
    # Crear cliente CoAP una sola vez
    context = await Context.create_client_context()

    while True:
        # Generar valores aleatorios
        datos = {
            "hum_a": random.randint(30, 40),   # Humedad aire
            "hum_s": random.randint(50, 70),   # Humedad suelo
            "level": random.randint(80, 120)   # Nivel de agua
        }

        payload = json.dumps(datos).encode("utf-8")

        request = Message(
            code=POST,
            payload=payload,
            uri=URI
        )

        print(f"\nEnviando: {datos}")

        try:
            response = await context.request(request).response
            print(f"Respuesta: {response.code}")
        except Exception as e:
            print(f"Error de conexión: {e}")

        # Esperar antes del siguiente envío
        await asyncio.sleep(INTERVALO)

if __name__ == "__main__":
    asyncio.run(enviar_datos())