import uwsgi
def application(e, sr):
    print(e)
    print e['wsgi.input'].read()
    if e['REQUEST_METHOD'] == 'ANNOUNCE':
        sr('200 OK', [('Cseq', e['HTTP_CSEQ'])])
        return ['']
    elif e['REQUEST_METHOD'] == 'OPTIONS':
        sr('200 OK', [('Cseq', e['HTTP_CSEQ']), ('Public', 'DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, RECORD')])
        return ['']
    elif e['REQUEST_METHOD'] == 'SETUP':
        sr('200 OK', [('Cseq', e['HTTP_CSEQ']), ('Transport', e['HTTP_TRANSPORT'])])
        return ['']
    elif e['REQUEST_METHOD'] == 'RECORD':
        sr('200 OK', [('Cseq', e['HTTP_CSEQ'])])
        uwsgi.add_var('STREAM', '1')
        return ['']
    return ['']
