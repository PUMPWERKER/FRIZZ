// masonry: each card goes into the shortest column, in order
(() => {
  const box = document.querySelector('.feats');
  const cards = [...box.querySelectorAll('.feat')];
  let count = 0;
  function layout() {
    // columns of at least 440px
    const n = Math.max(1, Math.floor((box.clientWidth + 24) / 464));
    if (n === count) return;
    count = n;
    const cols = Array.from({length: n}, () => Object.assign(document.createElement('div'), {className: 'col'}));
    box.replaceChildren(...cols);
    box.classList.add('masonry');
    for (const c of cards) cols.reduce((a, b) => b.offsetHeight < a.offsetHeight ? b : a).append(c);
  }
  layout();
  new ResizeObserver(() => layout()).observe(box);
  document.fonts?.ready.then(() => { count = 0; layout(); });
})();
