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

This plugin allows you to delegate a bunch of common realtime-related tasks to offload threads. It is a not silver bullet but instead tries to identify some common scenario for modern webapps.

How it works
============

The whole plugin is built around Redis publish/subscribe paradigm (in the future other queue engine could be added).

Generally, you offload a request instructing it to wait for messages coming from a redis queue. Whenever a message is received it is eventually manipulated and directly forwarded to the client.

All happens in background without using your workers. This means you can manage hundreds of sessions with a single worker.

Installation
============

The plugin is 2.0 compatible

The first example: SSE
======================

We want to "push" news to all connected clients (browsers) using SSE (HTML5 Server Sent Events).

Each client will subscribe to a "news" channel and will start waiting for messages directly enqueued in redis.

More SSE
========

Socket.io and Engine.io
=======================

Streaming
=========

Building an icecast2 compatible server
======================================

Notes
=====
