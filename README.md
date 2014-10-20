uwsgi-realtime
==============

*** WORK IN PROGRESS ***

a uWSGI plugin exposing offloaded realtime features like SSE, socket.io and media streaming

Why ?
=====

Having long-running connections opened in multiprocess/multithread contexts is bad.

Moving multiprocess/multithread-centric apps (like the vast majority of web frameworks out there like Django, Ruby On Rails, Catalyst ...) is hard (and sometime impossible) for various reasons.

Offloading is a pretty unique uWSGI feature allowing your app to delegate common/little tasks to one or more threads able to manage thousand of those tasks. This is possible because all of the enqueued tasks must be non-blocking.

In its core uWSGI allows the use of offloading for serving static files and for proxying between instances.

This plugin allows you to delegate a bunch of common realtime-related tasks to offload threads. It is not a silver bullet but instead tries to identify some common scenario for modern webapps.

How it works
============

The whole plugin is built around Redis publish/subscribe paradigm (in the future other queue engine could be added).

Generally, you offload a request instructing it to wait for messages coming from a redis queue. Whenever a message is received it is eventually manipulated and directly forwarded to the client.

All happens in background without using your workers. This means you can manage hundreds of sessions with a single worker.

Installation
============

You need routing enabled to use the plugin (if you get a warning about internal routing when starting uWSGI just rebuild it once you have installed libpcre development headers)

The plugin is 2.0 compatible

The first example: SSE
======================

Note: generally browsers do not allow for more than a couple of connections to the same resource, if you want to test with multiple clients use different browsers per-machine or directly multiple machines.

We want to "push" news to all connected clients (browsers) using SSE (HTML5 Server Sent Events).

Each client will subscribe to a "news" channel and will start waiting for messages directly enqueued in redis.

The HTML of the page is pretty simple, we subscribe to the '/news' url and we wait for messages

```html
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
</head>
<body>
  <script>
    var source = new EventSource('/news');
    source.onmessage = function(e) {
      document.body.innerHTML += e.data.replace(/\n/g, '<br/>') + '<hr>';
    };
  </script>
</body>
</html>
```

Save it as index.html

Now we spawn an instance with a routing rule delegating all requests for /news to the realtime offload engine

```ini
[uwsgi]
plugin = realtime
; offload requests for /news to the sse engine and instruct them to subscribe to the 'lotofnews' redis channel
route = ^/news$ sse:lotofnews
; bind to http port 9090
http-socket = :9090
; serve the html
static-map = /=index.html
offload-threads = 1
```

Connect to port 9090 with your browser and start publishing news:

(we use the 'redis-cli' client here, but could be anything allowed to publish to redis)

```sh
# redis-cli 
127.0.0.1:6379> publish lotofnews hello
```

If all goes well you should see 'hello' in your browser. Continue enqueing messages and have fun

More SSE
========

Socket.io and Engine.io
=======================

Raw Websockets
==============

HTML5 uploads
=============

Streaming
=========

Why limiting to text messages ? The redis pubsub system can carry binary data too, so we can use it to stream audio and video blobs.

RTSP
====

RTMPT
=====

FFmpeg chunked input
====================

ffmpeg can send frames via http using chunked input encoding. The webm container is a good one for streaming video
to web via the html5 tag.

On a macosx system you can send webm/vp8 video frames using:

```sh
ffmpeg -f avfoundation -i "0" -s 320x240 -r 30 -g 1 -b:v 1M http://address:port/stream.webm
```

(the -g parameter is the 'group of pictures', setting it to 1 ensure the video will be suddenly playable, increasing it will reduce data size but could result in pretty high delay. Try to experiment for the best value, but generally anything beteen 1 and 4 should be supported flawlessly)

The "istream" offload engine will detect the chunked input and will parse it generating a redis message for each chunk. Those chunks are fully valid webm clusters/blocks so you can stream the "as-is" to the client (you  only need to prepend them with the generic webm header, see below).

This is one of the tests provided with the uwsgi-realtime sources

```ini
plugin = realtime
http-socket = :9090
route = ^/stream.webm$ goto:stream
route-run = last:
static-map = /=tests/webm.html
offload-threads = 1

route-label = stream
; if GET stream the video
route-if = equal:${REQUEST_METHOD};GET webm:uwsgi
; if POST, capture frames and publish them to redis
route-if = equal:${REQUEST_METHOD};POST istream:uwsgi
```

You can now simply open with your video player (or with an html5 video tag) the /stream.webm resource

Building an icecast2 compatible server
======================================

WebM realtime streaming
=======================

Scaling streaming with redis
============================

Supported actions
=================

Notes
=====
