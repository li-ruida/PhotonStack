const steps = document.querySelectorAll('.steps a');
const preview = document.querySelector('#preview');
const previewLink = document.querySelector('#preview-link');
const caption = document.querySelector('#caption');

for (const step of steps) {
  step.addEventListener('click', (event) => {
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    preview.src = step.href;
    preview.alt = `PhotonStack ${step.textContent}界面。${step.dataset.caption}`;
    previewLink.href = step.href;
    caption.textContent = step.dataset.caption;
    for (const link of steps) link.removeAttribute('aria-current');
    step.setAttribute('aria-current', 'step');
  });
}
