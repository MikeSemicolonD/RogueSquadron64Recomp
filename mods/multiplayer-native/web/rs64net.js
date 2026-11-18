// The browser build's network on the page's main thread (--pre-js): one WebTransport session per remote ENet address, opened by
// web_socket.c with the certificate hash the relay printed. Datagrams go out as written and come back through _rs64_net_deliver.
Module.rs64Net = {
  sessions: new Map(),

  open(id, urlPtr, hexPtr)
  {
    const url = UTF8ToString(urlPtr);
    const hex = UTF8ToString(hexPtr);
    const session = { queue: [], writer: null, transport: null };

    Module._free(urlPtr);
    Module._free(hexPtr);
    this.sessions.set(id, session);

    const value = new Uint8Array(hex.match(/../g).map(h => parseInt(h, 16)));

    try {
      session.transport = new WebTransport(url, { serverCertificateHashes: [{ algorithm: 'sha-256', value }] });
    } catch (e) {
      console.warn(`[net] ${url}: ${e.message}`);
      this.sessions.delete(id);
      Module._rs64_net_closed(id);
      return;
    }

    session.transport.ready.then(async () => {
      session.writer = session.transport.datagrams.writable.getWriter();

      for (const d of session.queue)
        session.writer.write(d).catch(() => {});

      session.queue = [];

      const reader = session.transport.datagrams.readable.getReader();

      for (;;) {
        const { value: d, done } = await reader.read();

        if (done)
          break;

        const p = Module._malloc(d.length);

        HEAPU8.set(d, p);
        Module._rs64_net_deliver(id, p, d.length);
        Module._free(p);
      }
    }).catch(e => console.warn(`[net] ${url}: ${e.message}`)).finally(() => {
      this.sessions.delete(id);
      Module._rs64_net_closed(id);
    });
  },

  // Copies the datagram out of wasm memory (WebTransport takes no shared buffers) and frees the copy web_socket.c made.
  send(id, ptr, len)
  {
    const d = HEAPU8.slice(ptr, ptr + len);
    const session = this.sessions.get(id);

    Module._free(ptr);

    if (!session)
      return;

    if (session.writer)
      session.writer.write(d).catch(() => {});
    else
      session.queue.push(d);
  },

  close(id)
  {
    const session = this.sessions.get(id);

    if (session && session.transport)
      session.transport.close();
  }
};
