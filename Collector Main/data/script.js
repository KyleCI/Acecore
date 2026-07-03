async function updateMetrics() {
    try {
        const res = await fetch('/metrics');
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const data = await res.json();
        document.getElementById("powerOutput").innerText = (data.power_drawn ? `${(data.power_drawn / 1000).toFixed(2)} kW` : "-- kW");
        document.getElementById("energy").innerText = (data.energy_consumed ? `${data.energy_consumed.toFixed(2)} kWh` : "-- kWh");
        document.getElementById("energyRange").innerText = data.energy_consumed ? `${(data.energy_consumed * 1.1).toFixed(1)} kWh - ${(data.energy_consumed * 0.9).toFixed(1)} kWh` : "-- kWh - -- kWh";
    } catch (err) {
        console.error("Failed to fetch metrics:", err);
        document.getElementById("powerOutput").innerText = "-- kW";
        document.getElementById("energy").innerText = "-- kWh";
        document.getElementById("energyRange").innerText = "-- kWh - -- kWh";
    }
}

async function updateState() {
    try {
        const res = await fetch('/state');
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const state = await res.text();
        const stateDisplay = document.getElementById('stateDisplay');
        const powerBtn = document.getElementById('powerBtn');
        if (state === "ON" && stateDisplay.className !== "state on") {
            stateDisplay.textContent = "Powercell Active";
            stateDisplay.className = "state on";
            powerBtn.classList.add('active');
            powerBtn.classList.remove('red');
            if (!isTimerRunning) startTimer();
        } else if (state === "OFF" && stateDisplay.className !== "state off") {
            stateDisplay.textContent = "Powercell Inactive";
            stateDisplay.className = "state off";
            powerBtn.classList.remove('active');
            powerBtn.classList.add('red');
            resetTimer();
        }
    } catch (err) {
        console.error("Failed to fetch state:", err);
    }
}

let clickCount = 0, timerInterval, isTimerRunning = false, time = 0;
const powerBtn = document.getElementById('powerBtn');
const timerDisplay = document.getElementById('timer');

function startTimer() {
    if (isTimerRunning) return;
    isTimerRunning = true;
    timerInterval = setInterval(() => {
        time++;
        const h = String(Math.floor(time / 3600)).padStart(2, '0');
        const m = String(Math.floor((time % 3600) / 60)).padStart(2, '0');
        const s = String(time % 60).padStart(2, '0');
        timerDisplay.textContent = `${h} : ${m} : ${s}`;
    }, 1000);
}

function resetTimer() {
    clearInterval(timerInterval);
    isTimerRunning = false;
    time = 0;
    timerDisplay.textContent = '00 : 00 : 00';
}

powerBtn.addEventListener('click', async () => {
    clickCount = (clickCount + 1) % 2;
    const stateDisplay = document.getElementById('stateDisplay');
    try {
        if (clickCount === 1) {
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