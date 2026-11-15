const recipes = {
  m31: {
    total: '左侧为堆栈显影，右侧为最终成片。观察核心右侧的暗尘带，以及附近星点的颜色。',
    curves: '曲线前已中和色偏并轻度降噪。RGB 曲线将输入 0.05 提至 0.095，0.14 提至 0.26，提亮外盘，同时保留核心的亮度层次。',
    contrast: '局部对比强度 0.22，半径 18 像素。比较暗尘带与相邻明亮星盘的交界。',
    color: '饱和度增量 +0.70。比较星系核心的暖色与周围偏蓝的星点。'
  },
  mosaic: {
    total: '左侧为拼接显影，右侧为最终成片。比较星云弧线中红色细丝和青色边缘。',
    curves: '曲线前已轻度降噪。RGB 曲线将输入 0.05 提至 0.089，0.12 提至 0.26，让较暗的星云从背景中显现。',
    contrast: '局部对比强度 0.20，半径 18 像素。沿着明亮弧线观察相邻细丝之间的暗隙。',
    color: '饱和度增量 +0.90，使细丝中的红色与青色更易分辨。'
  }
};

const stages = {
  total: ['original', 'color'],
  curves: ['prepared', 'curves'],
  contrast: ['curves', 'contrast'],
  color: ['contrast', 'color']
};

function createComparison(example) {
  const comparison = document.querySelector('#finish-comparison').content.firstElementChild.cloneNode(true);
  const before = comparison.querySelector('.before');
  const after = comparison.querySelector('.after');
  const recipe = comparison.querySelector('.recipe');
  const curvePlot = comparison.querySelector('.curve-plot');
  const buttons = comparison.querySelectorAll('[data-stage]');
  const range = comparison.querySelector('.split');
  function selectStage(stage) {
    const [left, right] = stages[stage];
    before.src = `assets/finishing/${example}/${left}.webp`;
    after.src = `assets/finishing/${example}/${right}.webp`;
    curvePlot.hidden = stage !== 'curves';
    if (!curvePlot.hidden) curvePlot.src = `assets/finishing/${example}/curve.png`;
    recipe.textContent = recipes[example][stage];
    for (const button of buttons) button.setAttribute('aria-pressed', String(button.dataset.stage === stage));
  }
  for (const button of buttons) button.addEventListener('click', () => selectStage(button.dataset.stage));
  function positionDivider() {
    comparison.style.setProperty('--split', range.value + '%');
    range.setAttribute('aria-valuetext', `调整前 ${range.value}%，调整后 ${100 - range.value}%`);
  }
  range.addEventListener('input', positionDivider);
  positionDivider();
  selectStage('total');
  return comparison;
}

const workflows = document.querySelectorAll('input[name="workflow"]');
const finishSteps = new Map();
for (const gallery of document.querySelectorAll('.steps')) {
  const example = gallery.dataset.example;
  const figure = document.querySelector(`.workspace[data-example="${example}"]`);
  const previewLink = figure.querySelector('a');
  const preview = figure.querySelector('img');
  const caption = figure.querySelector('figcaption');
  const steps = gallery.querySelectorAll('a');
  let comparison;
  function select(step) {
    const finish = step.dataset.step === 'finish';
    if (finish && !comparison) {
      comparison = createComparison(example);
      figure.insertBefore(comparison, caption);
    }
    if (comparison) comparison.hidden = !finish;
    previewLink.hidden = finish;
    figure.classList.toggle('full-image', step.dataset.step === 'export');
    if (!finish) {
      preview.src = previewLink.href = step.href;
      preview.alt = step.dataset.caption;
      previewLink.setAttribute('aria-label', step.dataset.step === 'export' ? '查看完整彩色成片' : '放大查看当前步骤截图');
    }
    caption.textContent = step.dataset.caption;
    for (const link of steps) link.removeAttribute('aria-current');
    step.setAttribute('aria-current', 'step');
  }
  finishSteps.set(example, () => select(gallery.querySelector('[data-step="finish"]')));
  for (const step of steps) {
    step.addEventListener('click', event => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      event.preventDefault();
      select(step);
      const url = new URL(location.href);
      if (step.dataset.step === 'finish') url.searchParams.set('step', 'finish');
      else url.searchParams.delete('step');
      history.replaceState(null, '', url);
    });
  }
}

function selectFromURL() {
  const params = new URL(location.href).searchParams;
  const example = params.get('workflow') === 'mosaic' ? 'mosaic' : 'm31';
  for (const radio of workflows) radio.checked = radio.value === example;
  if (params.get('step') === 'finish') finishSteps.get(example)();
}
selectFromURL();
window.addEventListener('popstate', selectFromURL);
for (const radio of workflows) {
  radio.addEventListener('change', () => {
    const url = new URL(location.href);
    if (radio.value === 'm31') url.searchParams.delete('workflow');
    else url.searchParams.set('workflow', radio.value);
    const step = document.querySelector(`.steps[data-example="${radio.value}"] [aria-current]`);
    if (step.dataset.step === 'finish') url.searchParams.set('step', 'finish');
    else url.searchParams.delete('step');
    history.pushState(null, '', url);
  });
}
