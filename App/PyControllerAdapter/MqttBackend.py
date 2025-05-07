from fastapi import FastAPI, WebSocket
import json
import paho.mqtt.client as mqtt

app = FastAPI()

MQTT_BROKER = "sv02.tobicloud.eu"
MQTT_PORT = 1883
# Topics
TOPIC_DRIVE = "racecar/drive"
TOPIC_STEERING = "racecar/steering"

# MQTT Callback functions with debug outputs
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("MQTT: Connected to broker successfully!")
    else:
        print(f"MQTT: Failed to connect, return code {rc}")

def on_publish(client, userdata, mid):
    print(f"MQTT: Published message id {mid}")

mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_publish = on_publish

print("MQTT: Connecting to broker...")
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()

@app.post("/publish")
async def publish_message(payload: dict):
    topic = payload.get("topic")
    message = payload.get("message")
    if topic is None or message is None:
        return {"error": "Missing topic or message"}
    print(f"HTTP Publish: Publishing '{message}' to topic '{topic}'")
    mqtt_client.publish(topic, str(message))
    return {"status": "Published", "topic": topic, "message": message}

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    print("WebSocket: Client connected")
    try:
        while True:
            data = await websocket.receive_text()
            print(f"WebSocket: Received data {data}")
            payload = json.loads(data)
            topic = payload.get("topic")
            message = payload.get("message")
            if topic and message is not None:
                mqtt_client.publish(topic, str(message))
                response = f"Published {message} to {topic}"
                print(f"WebSocket: {response}")
                await websocket.send_text(response)
            else:
                error_response = "Missing topic or message"
                print(f"WebSocket: {error_response}")
                await websocket.send_text(error_response)
    except Exception as e:
        print(f"WebSocket: Exception occurred: {e}")
        await websocket.close()
        print("WebSocket: Connection closed")