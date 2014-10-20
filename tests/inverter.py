import redis

publisher = redis.StrictRedis()
subscriber = redis.StrictRedis().pubsub(ignore_subscribe_messages=True)
subscriber.subscribe('chat')

for message in subscriber.listen():
    if message['type'] == 'message':
        sender, body = message['data'].split(':', 2)
        publisher.publish('inverted', sender + ':' + body[::-1])
