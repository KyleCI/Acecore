function homeMetricNumber(...values) {
    for (const value of values) {
        if (value === null || value === undefined || value === "") continue;
        if (typeof value !== "number" && typeof value !== "string") continue;
        if (typeof value === "string" && value.trim() === "") continue;
        const number = Number(value);
        if (Number.isFinite(number)) return number;
    }
    return null;
}

async function updateMetrics() {
    try {
        const res = await fetch('/metrics', { cache: 'no-store' });
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const data = await res.json();

        // Use the same inverter readings displayed in the Powercell dashboard.
        // Keep the older field names as fallbacks for firmware compatibility.
        const powerWatts = homeMetricNumber(data.inverterLoad, data.inverter_output_load, data.power_drawn);
        const energyKwh = homeMetricNumber(data.inverter_energy, data.energy_consumed);

        document.getElementById("powerOutput").innerText = Number.isFinite(powerWatts)
            ? `${powerWatts.toFixed(2)} W`
            : "-- W";
        document.getElementById("energy").innerText = Number.isFinite(energyKwh)
            ? `${energyKwh.toFixed(2)} kWh`
            : "-- kWh";
        const energyRangeEl = document.getElementById("energyRange");
        if (energyRangeEl) {
            energyRangeEl.innerText = Number.isFinite(energyKwh)
                ? `${(energyKwh * 0.9).toFixed(1)} kWh - ${(energyKwh * 1.1).toFixed(1)} kWh`
                : "-- kWh - -- kWh";
        }
    } catch (err) {
        console.error("Failed to fetch metrics:", err);
        document.getElementById("powerOutput").innerText = "-- W";
        document.getElementById("energy").innerText = "-- kWh";
        const energyRangeEl = document.getElementById("energyRange");
        if (energyRangeEl) energyRangeEl.innerText = "-- kWh - -- kWh";
    }
}

async function updateState() {
    try {
        const res = await fetch('/state');
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const state = (await res.text()).trim().toUpperCase();
        const stateDisplay = document.getElementById('stateDisplay');
        const powerBtn = document.getElementById('powerBtn');
        if (state === "ON") {
            stateDisplay.textContent = "Powercell Active";
            stateDisplay.className = "state on";
            powerBtn.classList.add('active');
            powerBtn.classList.remove('red');
            if (!isTimerRunning) startTimer();
        } else if (state === "OFF") {
            stateDisplay.textContent = "Powercell Inactive";
            stateDisplay.className = "state off";
            powerBtn.classList.remove('active');
            powerBtn.classList.add('red');
            if (isTimerRunning || localStorage.getItem(TIMER_STARTED_AT_KEY)) {
                resetTimer();
            }
        }
    } catch (err) {
        console.error("Failed to fetch state:", err);
    }
}

const TIMER_STARTED_AT_KEY = 'powercellTimerStartedAt';
let timerInterval, isTimerRunning = false;
const powerBtn = document.getElementById('powerBtn');
const timerDisplay = document.getElementById('timer');

function renderTimer() {
    const startedAt = Number(localStorage.getItem(TIMER_STARTED_AT_KEY));
    const elapsedSeconds = Number.isFinite(startedAt) && startedAt > 0
        ? Math.max(0, Math.floor((Date.now() - startedAt) / 1000))
        : 0;
    const h = String(Math.floor(elapsedSeconds / 3600)).padStart(2, '0');
    const m = String(Math.floor((elapsedSeconds % 3600) / 60)).padStart(2, '0');
    const s = String(elapsedSeconds % 60).padStart(2, '0');
    timerDisplay.textContent = `${h} : ${m} : ${s}`;
}

function startTimer() {
    if (isTimerRunning) return;
    if (!localStorage.getItem(TIMER_STARTED_AT_KEY)) {
        localStorage.setItem(TIMER_STARTED_AT_KEY, String(Date.now()));
    }
    isTimerRunning = true;
    renderTimer();
    timerInterval = setInterval(renderTimer, 1000);
}

function resetTimer() {
    clearInterval(timerInterval);
    isTimerRunning = false;
    localStorage.removeItem(TIMER_STARTED_AT_KEY);
    timerDisplay.textContent = '00 : 00 : 00';
}

powerBtn.addEventListener('click', async () => {
    const stateDisplay = document.getElementById('stateDisplay');
    try {
        const isCurrentlyOn = stateDisplay.classList.contains('on');
        if (!isCurrentlyOn) {
            await fetch('/ON');
            stateDisplay.textContent = "Powercell Active";
            stateDisplay.className = "state on";
            powerBtn.classList.add('active');
            powerBtn.classList.remove('red');
            startTimer();
        } else {
            await fetch('/OFF');
            stateDisplay.textContent = "Powercell Inactive";
            stateDisplay.className = "state off";
            powerBtn.classList.remove('active');
            powerBtn.classList.add('red');
            resetTimer();
        }
    } catch (err) {
        console.error("Failed to toggle state:", err);
    }
});

function navigate(page) {
    window.location.href = page;
    const navItems = document.querySelectorAll('.nav-bottom a');
    navItems.forEach(item => {
        item.classList.remove('active');
        if (item.getAttribute('href') === page) {
            item.classList.add('active');
        }
    });
    navItems.forEach(item => {
        if (!item.classList.contains('active')) {
            item.style.color = '#888';
        } else {
            item.style.color = '#01937c';
        }
    });
}

function toggleMenu() {
    const navLinks = document.getElementById('navLinks');
    navLinks.classList.toggle('active');
}

window.onclick = function(event) {
    if (!event.target.matches('.menu-toggle') && !event.target.matches('.bi-list')) {
        const navLinks = document.getElementById('navLinks');
        if (navLinks.classList.contains('active')) {
            navLinks.classList.remove('active');
        }
    }
}

setInterval(updateMetrics, 5000);
setInterval(updateState, 5000);
updateMetrics();
updateState();
