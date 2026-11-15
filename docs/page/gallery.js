const workflows = document.querySelectorAll('input[name="workflow"]');

function selectFromURL() {
  const example = new URL(location.href).searchParams.get('workflow');
  for (const radio of workflows) {
    radio.checked = radio.value === (example === 'mosaic' ? 'mosaic' : 'm31');
  }
}

selectFromURL();
window.addEventListener('popstate', selectFromURL);
for (const radio of workflows) {
  radio.addEventListener('change', () => {
    const url = new URL(location.href);
    if (radio.value === 'm31') url.searchParams.delete('workflow');
    else url.searchParams.set('workflow', radio.value);
    history.pushState(null, '', url);
  });
}

for (const gallery of document.querySelectorAll('.steps')) {
  const figure = document.querySelector(`.workspace[data-example="${gallery.dataset.example}"]`);
  const previewLink = figure.querySelector('a');
  const preview = figure.querySelector('img');
  const caption = figure.querySelector('figcaption');
  const steps = gallery.querySelectorAll('a');

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
}
