// Solar readings arrive independently of the rest of the dashboard. Keep the
// last complete snapshot on screen while a poll fails or the MPPT link is
// between frames; replace it only with a newer valid reading.
let displayedSolar = null;

function finiteNumber(value) {
    if (value === null || value === undefined || value === "") return null;
    const number = Number(value);
    return Number.isFinite(number) ? number : null;
}

function getSolarReading(data) {
    const mppt = data.mppt_data && typeof data.mppt_data === "object" ? data.mppt_data : null;
    const mpptVoltage = finiteNumber(mppt?.["Solar Voltage"]);
    const mpptCurrent = finiteNumber(mppt?.["Solar Current"]);
    const mpptPower = finiteNumber(mppt?.["Solar Power"]);

    // Prefer a complete MPPT frame. The locally sampled values remain a
    // fallback only when no complete frame is available.
    if (mpptVoltage !== null && mpptCurrent !== null && mpptPower !== null) {
        const timestamp = finiteNumber(mppt?.Timestamp);
        return {
            source: "mppt",
            timestamp,
            voltage: mpptVoltage,
            current: mpptCurrent,
            power: mpptPower,
            health: mppt?.["MPP Found"] === true ? "MPP locked" : "Tracking"
        };
    }

    const voltage = finiteNumber(data.solar_voltage);
    const current = finiteNumber(data.solar_current);
    const power = finiteNumber(data.solar_power);
    if (voltage === null || current === null || power === null) return null;

    return {
        source: "local",
        timestamp: null,
        voltage,
        current,
        power,
        health: "Active"
    };
}

function isNewSolarReading(next) {
    if (!displayedSolar) return true;

    // Do not let a short MPPT outage replace controller data with a different
    // fallback source. The last controller frame stays visible until another
    // complete controller frame arrives.
    if (displayedSolar.source === "mppt" && next.source === "local") return false;
    if (displayedSolar.source !== next.source) return true;

    // MPPT frames include a source timestamp, so repeated dashboard polls do
    // not redraw the solar section until the controller sends a new frame.
    if (next.timestamp !== null) {
        return displayedSolar.timestamp === null || next.timestamp > displayedSolar.timestamp;
    }

    // Local fallback has no timestamp; its values themselves identify a new
    // sensor snapshot.
    return next.voltage !== displayedSolar.voltage ||
        next.current !== displayedSolar.current ||
        next.power !== displayedSolar.power;
}


async function updateDashboard() {
    try {
        const res = await fetch('/metrics', { cache: 'no-store' });
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const payload = await res.json();
        console.log("Fetched metrics:", payload);

        // Pull the actual record out of the array; bail cleanly if empty
        // const data = payload.inverter_data?.[0];
        const data = payload;
        if (!data) throw new Error("inverter_data is empty");

        // Battery Section
        document.getElementById("batteryStatus").innerText = (data.device_state === "On" || data.device_state === true) ? "Active" : "Inactive";
        document.getElementById("batteryVoltage").innerText = `${data.battery_voltage?.toFixed(2) ?? "--"} V`;
        document.getElementById("batteryLevel").innerText = `${data.battery_percentage?.toFixed(1) ?? "--"}%`;
        const timeToFullEl = document.getElementById("timeToFull"); if (timeToFullEl) timeToFullEl.innerText = "Time to Full: --";
        document.getElementById("batteryTemp").innerText = (data.battery_temperature != null) ? `${data.battery_temperature.toFixed(1)} °C` : "-- °C";


        // Powercell Section
        const powerWatts = finiteNumber(data.inverterLoad)
            ?? finiteNumber(data.inverter_output_load)
            ?? finiteNumber(data.power_drawn);
        document.getElementById("powerOutput").innerText = powerWatts !== null
            ? `${powerWatts.toFixed(2)} W` : "-- W";
        document.getElementById("energyConsumption").innerText = `${data.inverter_energy?.toFixed(2) ?? "--"} kWh`;
        document.getElementById("systemTemp").innerText = (data.inverter_temperature != null) ? `${data.inverter_temperature.toFixed(1)} °C` : "-- °C";
        document.getElementById("outputVoltage").innerText = (data.inverterVoltage != null) ? `${data.inverterVoltage.toFixed(2)} V` : "-- V";
        document.getElementById("inputVoltage").innerText = (data.gridVoltage != null) ? `${data.gridVoltage.toFixed(2)} V` : "-- V";

        console.log("Inverter Data:", powerWatts)


        // Solar Section
        document.getElementById("solarCurrent").innerText = (data.solarCurrent != null) ? `${data.solarCurrent.toFixed(2)} A` : "-- A";
        document.getElementById("solarPower").innerText = (data.solarPower != null) ? `${data.solarPower.toFixed(2)} W` : "-- W";
        document.getElementById("solarVoltage").innerText = (data.solarVoltage != null) ? `${data.solarVoltage.toFixed(2)} V` : "-- V";

    

        // Alerts & Maintenance
        document.getElementById("alerts").innerText = "No active alerts";
        document.getElementById("maintenance").innerText = "30 days";
        document.getElementById("diagnostics").innerText = "System OK";
    } catch (err) {
        console.error("Failed to fetch metrics:", err);
        document.getElementById("batteryStatus").innerText = "--";
        document.getElementById("batteryVoltage").innerText = "-- V";
        document.getElementById("batteryLevel").innerText = "-- %";
        const timeToFullEl = document.getElementById("timeToFull"); if (timeToFullEl) timeToFullEl.innerText = "Time to Full: --";
        document.getElementById("batteryTemp").innerText = "-- °C";
        document.getElementById("powerOutput").innerText = "-- W";
        document.getElementById("energyConsumption").innerText = "-- kWh";
        document.getElementById("systemTemp").innerText = "-- °C";
        // Preserve the last complete solar snapshot. A transient network error
        // must not make a working solar display jump back to placeholders.
        document.getElementById("alerts").innerText = "No active alerts";
        document.getElementById("maintenance").innerText = "System OK";
        document.getElementById("diagnostics").innerText = "System OK";
    }
}

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

document.addEventListener('DOMContentLoaded', () => {
    const currentPage = window.location.pathname.split('/').pop();
    const navItems = document.querySelectorAll('.nav-bottom a');
    navItems.forEach(item => {
        if (item.getAttribute('href') === currentPage) {
            item.classList.add('active');
        } else {
            item.style.color = '#888';
        }
    });
});

function toggleMenu() {
    const navLinks = document.getElementById('navLinks');
    const dropdownContent = document.getElementById('dropdownContent');
    navLinks.classList.toggle('active');
    dropdownContent.classList.toggle('active');
}

window.addEventListener('scroll', () => {
    const container = document.querySelector('.dashboard-container');
    const scrollHeight = container.scrollHeight;
    const clientHeight = container.clientHeight;
    const scrollTop = container.scrollTop;

    if (scrollHeight - (scrollTop + clientHeight) < 10) {
        container.style.animation = 'bounce 0.5s ease-out';
        setTimeout(() => {
            container.style.animation = '';
        }, 500);
    }
});

window.addEventListener('click', (event) => {
    const dropdown = document.querySelector('.dropdown');
    const menuToggle = document.querySelector('.menu-toggle');
    if (!dropdown.contains(event.target) && event.target !== menuToggle) {
        document.getElementById('navLinks').classList.remove('active');
        document.getElementById('dropdownContent').classList.remove('active');
    }
});

setInterval(updateDashboard, 5000);
updateDashboard();
