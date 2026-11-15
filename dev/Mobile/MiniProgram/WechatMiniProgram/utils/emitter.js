/**
 * 轻量事件总线（无第三方依赖）
 */
function createEmitter() {
  const handlers = {};
  return {
    on(event, fn) {
      (handlers[event] || (handlers[event] = [])).push(fn);
      return () => this.off(event, fn);
    },
    off(event, fn) {
      const list = handlers[event];
      if (!list) return;
      const i = list.indexOf(fn);
      if (i > -1) list.splice(i, 1);
    },
    emit(event, payload) {
      const list = handlers[event];
      if (!list) return;
      list.slice().forEach((fn) => {
        try { fn(payload); } catch (e) { console.error('[emitter]', event, e); }
      });
    },
  };
}

module.exports = createEmitter();
