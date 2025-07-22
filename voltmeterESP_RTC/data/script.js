const simulated = false;

let isDragging = false;
let startX = 0;
let currentX = 0;
let isAnimating = false;
let activeSlide = 0;
let userOverridden = false;
let overrideTimeout = null;
let voltageChart = null;
let capacityChart = null;
let kwhChart = null;
let ampsChart = null;
let slideIndex = 0; // or whatever the default is



const simulatedData = {
  now: Math.floor(Date.now() / 1000),
  batteryType: 0,
  cellcount: 4,
  watts: 123,
  Ampere: 0.67,
  cur_dir: 1,
  voltage: 14.8,
  batteryCapacityAh: 20.0,
  remainingCapacityAh: 19.0,
  pricePerKWh: 0.25,
  totalWh: 123.5,
  totalKWh: 0.235,
  capacity: 0,
  screen: 0, 
  hourlyKWh: [
    0.00, 0.02, 0.06, 0.10, 0.14, 0.17, 
    0.175, 0.17, 0.14, 0.10, 0.05, 0.01
  ],
    historyCapacity: [
    100, 99, 98, 97, 96, 95, 94, 93, 92, 91,
    90, 89, 88, 87, 86, 85, 84, 83, 82, 81,
    80, 79, 78, 76, 74, 72, 70, 68, 66, 64,
    62, 60, 58, 56, 54, 52, 50, 48, 46, 44,
    42, 40, 38, 36, 34, 32, 30, 28, 27, 26,
    25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
    15, 14, 13, 12, 11, 10, 9, 9, 9, 9,
    9, 9
  ],
  historyVoltage: Array.from({ length: 72 }, () => +(10.2 + Math.random() * 2.0).toFixed(2)),
  maxA: 12.34,
  maxA_min: 9.87,
  usedEnergy: 800,
  usedUnit: "mAh",
  last60Amps: Array.from({ length: 60 }, (_, i) => (Math.random() * 10).toFixed(2)),
  maxWatts: 265
  // dummy amp values for past 60s
};




const slider = document.getElementById("slider");

// === Utility: Update Pie Chart ===
function drawPieChart(percent) {
  const donut = document.getElementById("donutChart");

  let color = "#5CD66E"; // green
  if (percent < 20) {
    color = "#E24F4F"; // red
  } else if (percent < 50) {
    color = "#ff9800"; // orange
  }
  
  donut.style.background = `conic-gradient(${color} 0% ${percent}%, #ddd ${percent}% 100%)`;

  const centerText = document.getElementById("capacity");
  centerText.textContent = Math.round(percent);
}







// === Responsive Slide Control ===
let slideUpdateTimeout = null;

slider.addEventListener('transitionend', (e) => {
  if (e.propertyName === 'transform') {
    isAnimating = false;
    slider.style.transition = 'none';
  }
});

function setSlide(index, fromUser = false) {
  activeSlide = Math.max(0, Math.min(index, 2));
  slideIndex = activeSlide; // ✅ Set global slideIndex here
 // console.log("slideIndex:", slideIndex);

  isAnimating = true;

  const gap = 0;
  slider.style.transition = 'transform 0.3s ease';
  slider.style.transform = `translateX(calc(-${activeSlide * 100}% - ${activeSlide * gap}px))`;
  document.getElementById("screen").textContent = activeSlide;

  // Update button states
  document.querySelectorAll(".slider-controls button").forEach((btn, i) => {
    btn.classList.toggle("active", i === activeSlide);
  });

  // Update container height
  updateSliderHeight();

  // Update ESP
  if (fromUser) {
    userOverridden = true;
    clearTimeout(overrideTimeout);
    overrideTimeout = setTimeout(() => userOverridden = false, 5000);

    clearTimeout(slideUpdateTimeout);
    slideUpdateTimeout = setTimeout(() => {
      fetch('/setScreen', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `value=${activeSlide}`
      })
      //.catch(err => console.error("ESP update failed:", err));
    }, 200);
  }
}



// === Fetch Data from ESP ===
function fetchData() {
  if (simulated) {
    applyData(simulatedData);
    setSlide(simulatedData.screen);  
    // Also render charts with simulated data and simulated timestamp
    renderVoltageChart(simulatedData.historyVoltage, simulatedData.now);
    renderCapacityChart(simulatedData.historyCapacity, simulatedData.now);
    // Call your other chart rendering functions here, passing simulatedData and simulatedData.now
    return;
  }

  fetch('/data')
    .then(response => {
      if (!response.ok) throw new Error("Network error");
      return response.json();
    })
    .then(data => {
      applyData(data);

      // Update slide based on screen value if not overridden by user
      if (!userOverridden && typeof data.screen === "number") {
        setSlide(data.screen, false); // false disables user animation + avoids lock
        isAnimating = false;          // just to be extra safe
      }

      // Render charts with fresh data & synced time
      renderVoltageChart(data.historyVoltage, data.now);
      renderCapacityChart(data.historyCapacity, data.now);
      // Call your other chart rendering functions here, passing 'data' and 'data.now'

    })
    .catch(error => console.error("Fetch failed:", error));
}





function destroyVoltageChart() {
  if (voltageChart) {
    voltageChart.destroy();
    voltageChart = null;
  }
}

function destroyCapacityChart() {
  if (capacityChart) {
    capacityChart.destroy();
    capacityChart = null;
  }
}

function destroyKWhChart() {
  if (kwhChart) {
    kwhChart.destroy();
    kwhChart = null;
  }
}

function destroyLast60AmpsChart() {
  if (ampsChart) {
    ampsChart.destroy();
    ampsChart = null;
  }
}




function applyData(data) {
  // Helper: destroy only charts not relevant to the current slide
  function destroyInactiveChartsForSlide(slideIndex) {
    if (slideIndex !== 0) {
      destroyVoltageChart?.();
      destroyCapacityChart?.();
    }
    if (slideIndex !== 1) {
      destroyKWhChart?.();
    }
    if (slideIndex !== 2) {
      destroyLast60AmpsChart?.();
    }
  }

  // Battery Type Label
  const batteryTypeLabels = ["LiIon", "LiPo", "LiFePO4"];
  const batteryTypeIndex = parseInt(data.batteryType);
  document.getElementById("batteryType").textContent = batteryTypeLabels[batteryTypeIndex] || "Unknown";

  document.getElementById("cellcount").textContent = data.cellcount;
  document.querySelectorAll(".watts").forEach(el => el.textContent = data.watts);
  document.getElementById("cur_dir").textContent = data.cur_dir;
  document.querySelectorAll(".ampere").forEach(el => {
    const ampValue = Number(data.Ampere);
    const isNegative = data.cur_dir === 1 || data.cur_dir === "1";
    el.textContent = (isNegative ? -ampValue : ampValue).toFixed(2);
  });
  document.querySelectorAll(".voltage").forEach(el => el.textContent = data.voltage.toFixed(2));
  document.getElementById("batteryCapacityAh").textContent = data.batteryCapacityAh.toFixed(2);
  document.getElementById("remainingCapacityAh").textContent = data.remainingCapacityAh.toFixed(2);
  document.getElementById("pricePerKWh").textContent = data.pricePerKWh.toFixed(3);

  const totalWh = data.totalWh;
  if (totalWh >= 1000) {
    document.getElementById("totalWh").textContent = (totalWh / 1000).toFixed(2);
    document.getElementById("totalWhValue").textContent = " kWh";
  } else {
    document.getElementById("totalWh").textContent = totalWh.toFixed(2);
    document.getElementById("totalWhValue").textContent = " Wh";
  }

  if (data.usedEnergy != null) {
    let value = data.usedEnergy;
    let unit = data.usedUnit || "Ah";
    if (unit === "Ah" && Math.abs(value) < 1) {
      value *= 1000;
      unit = "mAh";
    }
    const formattedValue = value.toFixed(unit === "Ah" ? 2 : 0);
    document.querySelectorAll(".usedAh").forEach(el => el.textContent = formattedValue);
    document.querySelectorAll(".usedAh_label").forEach(el => el.textContent = unit);
  } else {
    document.querySelectorAll(".usedAh").forEach(el => el.textContent = "0");
    document.querySelectorAll(".usedAh_label").forEach(el => el.textContent = "mAh");
  }

  document.getElementById("capacity").textContent = data.capacity.toFixed(0);
  if (typeof drawPieChart === "function") drawPieChart(data.capacity);

  if (!userOverridden && data.screen !== activeSlide) {
    setSlide(data.screen);
  }

  const totalPrice = (data.totalKWh * data.pricePerKWh).toFixed(2);
  document.getElementById("totalPrice").textContent = totalPrice;

  const directionElements = document.querySelectorAll(".direction");
  directionElements.forEach(el => {
    el.classList.remove("forward", "backward");
    if (data.cur_dir == 1) el.classList.add("backward");
    else if (data.cur_dir == 2) el.classList.add("forward");
  });

  // Destroy inactive charts safely
  destroyInactiveChartsForSlide(activeSlide);

  // Render the active chart(s)
  if (activeSlide === 0) {
    renderVoltageChart(Array.isArray(data.historyVoltage) ? data.historyVoltage : [], data.now);
    renderCapacityChart(Array.isArray(data.historyCapacity) ? data.historyCapacity : [], data.now);
  } else if (activeSlide === 1) {
    renderKWhBarChart(Array.isArray(data.hourlyKWh) ? data.hourlyKWh : [], data.now);
  } else if (activeSlide === 2) {
    if (Array.isArray(data.last60Amps)) {
      renderLast60AmpsChart(data.last60Amps);
    }
  }

  const hourlyKWhArray = Array.isArray(data.hourlyKWh) ? data.hourlyKWh : [];
  const sumHourlyKWh = hourlyKWhArray.reduce((sum, val) => sum + val, 0);
  document.getElementById("hourlyKWhTotal").textContent = sumHourlyKWh.toFixed(3);

  // Remaining Time Calculation
  const amp = Number(data.Ampere);
  const capacity = Number(data.batteryCapacityAh);
  const remaining = Number(data.remainingCapacityAh);
  const direction = Number(data.cur_dir);

  let hoursText = "00";
  let minutesText = "00";
  let labelText = "Time:";
  let timeHours = null;

  if (amp > 0 && direction !== 0) {
    if (direction === 2) {
      timeHours = (capacity - remaining) / amp;
      labelText = "Estimated charge time:";
    } else if (direction === 1) {
      timeHours = remaining / amp;
      labelText = "Estimated discharge time:";
    }

    if (isFinite(timeHours) && timeHours >= 0) {
      const h = Math.floor(timeHours);
      const m = Math.round((timeHours - h) * 60);
      hoursText = h.toString().padStart(2, "0");
      minutesText = m.toString().padStart(2, "0");
    }
  } else {
    labelText = "System idle";
  }

  document.getElementById("hours").textContent = hoursText + "h ";
  document.getElementById("minutes").textContent = minutesText + "m";
  document.getElementById("timeLabel").textContent = labelText;

  // Peak values
  if (data.maxA !== undefined) {
    document.getElementById("maxA").textContent = data.maxA.toFixed(2);
  }

  if (data.maxA_min !== undefined) {
    document.getElementById("maxA_min").textContent = data.maxA_min.toFixed(3);
  }

  if (data.maxWatts !== undefined) {
    document.querySelectorAll(".maxWatts").forEach(el => el.textContent = data.maxWatts.toFixed(0) + " W");
  }
}


function updateSliderHeight() {
  const sliderContainer = document.querySelector('.slider-container');
  const activeSlideElement = slider.children[activeSlide];
  if (activeSlideElement && sliderContainer) {
    const newHeight = activeSlideElement.scrollHeight;
    sliderContainer.style.height = newHeight + 'px';
  }
}




// === Manual Slide Buttons ===
document.querySelectorAll(".slider-controls button").forEach((btn, i) => {
  btn.addEventListener("click", () => setSlide(i));
});

// === Accordion Logic ===
document.querySelectorAll(".accordion").forEach(acc => {
  acc.addEventListener("click", () => {
    const panel = acc.nextElementSibling;
    const isActive = acc.classList.contains("active");

    // Close all
    document.querySelectorAll(".accordion").forEach(el => el.classList.remove("active"));

    if (!isActive) {
      acc.classList.add("active");
    }

    // Immediately adjust slider container height
    updateSliderHeight();
  });
});

// === Drag Gesture for Slides ===


const threshold = 150; // px threshold for starting slide drag

function onStart(e) {
  if (isAnimating) {
    console.log("Blocked: still animating");
    return;
  }
    //if (e.target.closest('.accordion') || e.target.closest('.panel')) return;
    const tag = e.target.tagName.toLowerCase();
    if (['button', 'input', 'canvas'].includes(tag)) return;

  isDragging = true;
  startX = e.touches ? e.touches[0].clientX : e.clientX;
  currentX = startX;
  slider.style.transition = 'none'; // Disable transition during drag
}

function onMove(e) {
  if (!isDragging) return;
  currentX = e.touches ? e.touches[0].clientX : e.clientX;
  const dx = currentX - startX;

  const baseTranslate = -activeSlide * 100;
  if (Math.abs(dx) >= threshold) {
    slider.style.transform = `translateX(calc(${baseTranslate}% + ${dx}px))`;
  } else {
    slider.style.transform = `translateX(${baseTranslate}%)`;
  }
}

function onEnd(e) {
  if (!isDragging) return;
  isDragging = false;
  //slider.style.transition = 'transform 0.3s ease'; // Re-enable transition for snapping

  const dx = currentX - startX;
  const maxSlideIndex = 2;

  if (dx < -threshold && activeSlide < maxSlideIndex) {
    setSlide(activeSlide + 1, true);
  } else if (dx > threshold && activeSlide > 0) {
    setSlide(activeSlide - 1, true);
  } else {
    setSlide(activeSlide, true);
  }

}

const sliderContainer = document.querySelector('.slider-container');

sliderContainer.addEventListener('touchstart', onStart);
sliderContainer.addEventListener('touchmove', onMove);
sliderContainer.addEventListener('touchend', onEnd);

sliderContainer.addEventListener('mousedown', onStart);
sliderContainer.addEventListener('mousemove', onMove);
sliderContainer.addEventListener('mouseup', onEnd);
sliderContainer.addEventListener('mouseleave', onEnd);


// === Start App After DOM Ready ===
window.addEventListener("DOMContentLoaded", () => {
  document.getElementById("capacity").textContent = simulatedData.capacity;
  drawPieChart(simulatedData.capacity);

  // Disable animation on first load
  slider.style.transition = 'none';
  setSlide(simulatedData.screen, false);

  // Manually clear animation state since no transitionend will fire
  isAnimating = false;

  renderVoltageChart(simulatedData.historyVoltage);
  renderCapacityChart(simulatedData.historyCapacity);
  renderKWhBarChart(simulatedData.hourlyKWh);
  renderLast60AmpsChart(simulatedData.last60Amps);

  fetchData();
});

const fetchDataInterval = setInterval(fetchData, 1000);
//console.log("Sending slide update to ESP:", activeSlide);


window.addEventListener("beforeunload", () => {
  clearInterval(fetchDataInterval);
  clearTimeout(overrideTimeout);
  clearTimeout(slideUpdateTimeout);
});



const chartCanvas = document.getElementById('capacityChart');
const chartCanvas2 = document.getElementById('myChart');
const chartCanvas3 = document.getElementById('ampsChart');
const chartCanvas4 = document.getElementById('voltageChart');


['mousedown', 'touchstart', 'mousemove', 'touchmove', 'mouseup', 'touchend', 'click'].forEach(eventType => {
  chartCanvas.addEventListener(eventType, e => {
    e.stopPropagation();
  });
  chartCanvas2.addEventListener(eventType, e => {
    e.stopPropagation();
  });
  chartCanvas3.addEventListener(eventType, e => {
    e.stopPropagation();
  });
  chartCanvas4.addEventListener(eventType, e => {
    e.stopPropagation();
  });
});







//BAR
const verticalLinePlugin = {
  id: 'verticalLine',
  afterDraw: (chart) => {
    if (chart.tooltip._active && chart.tooltip._active.length) {
      const ctx = chart.ctx;
      const activePoint = chart.tooltip._active[0];
      const x = activePoint.element.x;
      const y = activePoint.element.y;
      const topY = chart.chartArea.top;
      const bottomY = chart.chartArea.bottom;

      // Draw vertical dashed line
      ctx.save();
      ctx.beginPath();
      ctx.moveTo(x, topY);
      ctx.lineTo(x, bottomY);
      ctx.lineWidth = 1;
      ctx.strokeStyle = '#fff'; // match chart color
      ctx.setLineDash([]); // solid line
      ctx.stroke();
      ctx.restore();

    
      // Draw custom dot at intersecting point
      ctx.save();
      ctx.beginPath();
      ctx.arc(x, y, 4, 0, 2 * Math.PI); // radius 5
      if (slideIndex === 2) {
        ctx.fillStyle = '#2196f3';       // Fill with green

      }
      else {
        ctx.fillStyle = '#5CD66E';       // Fill with green
      }
      ctx.strokeStyle = '#fff'; // green outline
      ctx.lineWidth = 2;
      ctx.fill();
      ctx.stroke();
      ctx.restore();
    }
  }
};


Chart.register({
  id: 'customTooltip',
  afterDraw(chart) {
    const tooltip = chart.tooltip;
    const canvas = chart.canvas;
    const container = canvas.closest('.chartcontainer');
    if (!container) return;

    const tooltipEl = container.querySelector('.chart-tooltip');
    if (!tooltipEl) return;

    if (!tooltip || !tooltip.getActiveElements().length) {
      tooltipEl.style.display = 'none';
      return;
    }

    const { caretX, caretY, dataPoints } = tooltip;
    const firstPoint = dataPoints[0];
    const xLabel = firstPoint?.label || '';

    // Header with time label like "1h 10m ago"
    let innerHtml = `<div style="margin-bottom: 4px;"><strong>${xLabel}</strong></div>`;

    dataPoints.forEach(dp => {
      const label = dp.dataset.label || '';
      const value = dp.formattedValue || dp.raw;

      // Guess unit
      let unit = '';
      const match = label.match(/\(([^)]+)\)/);
      if (match) {
        unit = match[1];
      } else if (label.toLowerCase().includes('volt')) unit = 'V';
      else if (label.toLowerCase().includes('amp')) unit = 'A';
      else if (label.toLowerCase().includes('wh')) unit = 'Wh';
      else if (label.toLowerCase().includes('capacity')) unit = '%';

      innerHtml += `<div>${label}: ${value} ${unit}</div>`;
    });

    tooltipEl.innerHTML = innerHtml;

    // Position inside the chart container
    tooltipEl.style.left = caretX + -35 + 'px';
    tooltipEl.style.top = caretY + 0 + 'px';
    tooltipEl.style.display = 'block';
  }
});






function destroyInactiveCharts(active) {
  if (active !== 'voltage' && voltageChart) {
    voltageChart.destroy();
    voltageChart = null;
  }
  if (active !== 'capacity' && capacityChart) {
    capacityChart.destroy();
    capacityChart = null;
  }
  if (active !== 'kwh' && kwhChart) {
    kwhChart.destroy();
    kwhChart = null;
  }
  if (active !== 'amps' && ampsChart) {
   ampsChart.destroy();
   ampsChart = null;
  }
}


// Utility: Generate time labels for line/bar charts (UTC)
function generateTimeLabels(nowSecs, points, intervalSec, roundToNearest = false) {
  let baseDate = new Date(nowSecs * 1000);

  console.log("nowSecs (Unix seconds):", nowSecs);
  console.log("Rounded base time (UTC):", roundToNearest10MinUTC(new Date(nowSecs * 1000)));

  if (roundToNearest) {
    baseDate = roundToNearest10MinUTC(baseDate);
  }
  const baseMs = baseDate.getTime();

  return Array.from({ length: points }, (_, i) => {
    const offsetMs = (points - 1 - i) * intervalSec * 1000;
    const t = new Date(baseMs - offsetMs);
    const hh = String(t.getUTCHours()).padStart(2, '0');
    const mm = String(t.getUTCMinutes()).padStart(2, '0');
    return `${hh}:${mm}`;
  });
}

// Utility: Round a Date object down to nearest 10-minute boundary (UTC)
function roundToNearest10MinUTC(date) {
  date = new Date(date.getTime());
  date.setUTCSeconds(0, 0);
  const minutes = date.getUTCMinutes();
  date.setUTCMinutes(minutes - (minutes % 10));
  return date;
}



// Voltage Chart
function renderVoltageChart(data, nowSecs) {
  const ctx = document.getElementById('voltageChart').getContext('2d');

  const voltageData = Array.isArray(data) && data.length === 72
    ? data.map(v => parseFloat(v) || 0)
    : Array(72).fill(0);

  const labels = generateTimeLabels(nowSecs, 72, 600, true); // 600s = 10min, rounded

  // Gradient
  const gradient = ctx.createLinearGradient(0, 0, 0, ctx.canvas.height);
  gradient.addColorStop(0, 'rgba(92, 214, 110, 0.8)');
  gradient.addColorStop(1, 'rgba(0, 0, 0, 0)');

  if (voltageChart) {
    voltageChart.data.labels = labels;
    voltageChart.data.datasets[0].data = voltageData;
    voltageChart.update();
    return;
  }

  voltageChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels,
      datasets: [{
        label: 'Voltage',
        data: voltageData,
        borderColor: '#5CD66E',
        backgroundColor: gradient,
        fill: true,
        pointRadius: 0,
        borderWidth: 2,
        tension: 0.25,
        pointBackgroundColor: '#5CD66E'
      }]
    },
    options: {
      responsive: true,
      animation: true,
      interaction: { mode: 'index', intersect: false },
      scales: {
        y: { beginAtZero: false },
        x: {
          grid: { display: false },
          border: { display: true, color: '#999' },
          ticks: { maxTicksLimit: 6 }
        }
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          enabled: false,
          mode: 'index',
          intersect: false,
          position: 'nearest',
          external: () => {},
          callbacks: {
            label: ctx => `Voltage: ${ctx.parsed.y.toFixed(2)} V`
          }
        }
      }
    },
    plugins: [verticalLinePlugin]
  });
}

// Capacity Chart
function renderCapacityChart(data, nowSecs) {
  const ctx = document.getElementById('capacityChart').getContext('2d');

  const capacityData = Array.isArray(data) && data.length === 72
    ? data.map(v => v || 0)
    : Array(72).fill(0);

  const labels = generateTimeLabels(nowSecs, 72, 600, true);

  const colors = capacityData.map(v => {
    if (v < 20) return "#E24F4F";
    if (v < 50) return "#ff9800";
    return "#5CD66E";
  });

  if (capacityChart) {
    capacityChart.data.labels = labels;
    capacityChart.data.datasets[0].data = capacityData;
    capacityChart.data.datasets[0].backgroundColor = colors;
    capacityChart.update();
    return;
  }

  capacityChart = new Chart(ctx, {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        label: 'Capacity',
        data: capacityData,
        backgroundColor: colors,
        fill: true,
        pointRadius: 1,
        tension: 0.25,
        pointBackgroundColor: colors
      }]
    },
    options: {
      responsive: true,
      animation: true,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        tooltip: {
          enabled: false,
          external: () => {},
          mode: 'index',
          intersect: false,
          position: 'nearest',
          padding: 10,
          yAlign: 'bottom',
          caretPadding: 50,
          displayColors: false,
          cornerRadius: 6,
          titleFont: { size: 12, weight: 'bold', family: 'Arial' },
          bodyFont: { size: 12, family: 'Arial' },
          titleMarginBottom: 0,
          callbacks: {
            label: context => `Capacity ${context.parsed.y.toFixed(0)} %`
          }
        },
        legend: { display: false }
      },
      scales: {
        y: {
          min: 0,
          max: 100,
          beginAtZero: true,
          grace: '10%'
        },
        x: {
          grid: { display: false },
          border: { display: true, color: '#999999' },
          ticks: { display: true, maxTicksLimit: 6 }
        }
      }
    },
    plugins: [verticalLinePlugin]
  });
}

// kWh Chart
function renderKWhBarChart(data, nowSecs) {
  const ctx = document.getElementById('myChart').getContext('2d');

  const kwhData = Array.isArray(data) && data.length
    ? data.slice(-12).map(v => Math.round((v || 0) * 1000)) // kWh → Wh
    : Array(12).fill(0);

  const labels = generateTimeLabels(nowSecs, 12, 3600, true).map(label => label.replace(/:\d{2}$/, ':00')); // round to hour

  if (kwhChart) {
    kwhChart.data.labels = labels;
    kwhChart.data.datasets[0].data = kwhData;
    kwhChart.update();
    return;
  }

  kwhChart = new Chart(ctx, {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        label: 'Wh',
        data: kwhData,
        backgroundColor: Array(12).fill('#4caf50'),
        borderWidth: 0
      }]
    },
    options: {
      responsive: true,
      animation: true,
      onHover: (event, elements) => {
        const dataset = kwhChart.data.datasets[0];
        if (elements.length > 0) {
          const hoveredIndex = elements[0].index;
          dataset.backgroundColor = dataset.data.map((_, idx) =>
            idx === hoveredIndex ? '#4caf50' : 'rgba(255, 255, 255, 0.1)'
          );
        } else {
          dataset.backgroundColor = dataset.data.map(() => '#4caf50');
        }
        kwhChart.update('none');
      },
      scales: {
        y: {
          beginAtZero: true,
          grace: '10%'
        },
        x: {
          grid: { display: false },
          border: { display: true, color: '#999' },
          ticks: {
            display: true,
            maxTicksLimit: 6
          }
        }
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          enabled: false,
          mode: 'index',
          intersect: false,
          position: 'nearest',
          external: () => {}
        }
      }
    }
  });
}




// AMPS Chart instance



function renderLast60AmpsChart(data) {
  const ctx = document.getElementById('ampsChart').getContext('2d');

  // Create a vertical gradient (top to bottom)
const gradient = ctx.createLinearGradient(0, 0, 0, ctx.canvas.height);
gradient.addColorStop(0, 'rgba(33, 150, 243, 0.8)');   // Top
gradient.addColorStop(1, 'rgba(0, 0, 0, 0)');     // Bottom




  // Use last 60 or fill with zeros
  const ampsData = Array.isArray(data) && data.length
    ? data.slice(-60).map(v => v || 0)
    : Array(60).fill(0);

  const labels = Array.from({ length: 60 }, (_, i) => `${59 - i}s ago`);


 
  if (ampsChart) {
    ampsChart.data.labels = labels;
    ampsChart.data.datasets[0].data = ampsData;
    ampsChart.update();
  } else {
    ampsChart = new Chart(ctx, {
      type: 'line',
      data: {
        labels,
        datasets: [{
          label: 'Amps',
          data: ampsData,
          borderColor: '#2196f3',
         // backgroundColor: 'rgba(33, 150, 243, 0.1)',
          backgroundColor: gradient, // ✅ Use the gradient here

          fill: true,
          borderWidth: 2,
          pointBackgroundColor:'#2196f3',
          pointRadius: 0,
          tension: 0.25
        }]
      },
      options: {
        responsive: true,
        animation: false, // Disables animation completely
        interaction: {
          mode: 'index',         // <- highlight all points at index
          intersect: false       // <- trigger highlight even if not exactly over a point
        },
        
        scales: {
          y: {
            beginAtZero: true,
            title: {
              display: false,
              text: 'Amps'
            }
          },
          x: {
            title: {
              display: false,
              text: 'Seconds ago'
            },
            grid: {
              display: false,  // No vertical lines
              drawTicks: false // Optional: hide tick marks
            },
            border: {
              display: true,
              color: '#999999' 
            },
            ticks: {
              display: false,
              maxTicksLimit: 4,
              
            }
          }
        },
        plugins: {
          legend: {
            display: false
          },
          tooltip: {
            enabled: false, // Disable default tooltip
            external: () => {}, // Prevent fallback behavior
            mode: 'index',
          intersect: false,
          position: 'nearest',
            padding: 10,
            cornerRadius: 6,
            yAlign: 'bottom',
            caretPadding: 50,
            displayColors: false,
            titleFont: {
              size: 12,
              weight: 'bold',
              family: 'Arial'
            },
            bodyFont: {
              size: 12,
              family: 'Arial'
            },
            titleMarginBottom: 0,
            callbacks: {
              label: context => `Load ${context.parsed.y} A`
            }
          }
        }
      },
      plugins: [verticalLinePlugin]
    });
  }
}


