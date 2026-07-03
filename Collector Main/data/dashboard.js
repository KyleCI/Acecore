async function updateDashboard() {
    try {
        const res = await fetch('/metrics');
        if (!res.ok) throw new Error(`HTTP error: ${res.status}`);
        const payload = await res.json();
        console.log("Fetched metrics:", payload);

        // Pull the actual record out of the array; bail cleanly if empty
        // const data = payload.inverter_data?.[0];
        const data = payload;
        if (!data) throw new Error("inverter_data is empty");

        // mppt_data can be null when MPPT isn't reporting — default to {}
        const mppt = data.mppt_data ?? {};

        // Battery Section
        document.getElementById("batteryStatus").innerText = (data.device_state === "On" || data.device_state === true) ? "Active" : "Inactive";
        document.getElementById("batteryVoltage").innerText = `${data.battery_voltage?.toFixed(2) ?? "--"} V`;
        document.getElementById("batteryLevel").innerText = `${data.battery_percentage?.toFixed(1) ?? "--"}%`;
        document.getElementById("timeToFull").innerText = "Time to Full: --";
        document.getElementById("batteryTemp").innerText = (data.battery_temperature != null) ? `${data.battery_temperature.toFixed(1)} °C` : "-- °C";

        // Powercell Section
        document.getElementById("powerOutput").innerText = `${data.inverter_output_load?.toFixed(2) ?? "--"} W`;
        document.getElementById("energyConsumption").innerText = `${data.inverter_energy?.toFixed(2) ?? "--"} kWh`;
        document.getElementById("systemTemp").innerText = (data.inverter_temperature != null) ? `${data.inverter_temperature.toFixed(1)} °C` : "-- °C";

        // Solar Panel Section — all guarded through `mppt`
        const sv = mppt["Solar Voltage"];
        const sc = mppt["Solar Current"];
        const sp = mppt["Solar Power"];
        document.getElementById("solarEnergy").innerText  = (sp != null) ? `${(data.energy_consumed * 10 / 1000).toFixed(2)} kWh` : "-- kWh";
        document.getElementById("solarVoltage").innerText = (sv != null) ? `${sv.toFixed(2)} V` : "-- V";
        document.getElementById("solarCurrent").innerText = (sc != null) ? `${sc.toFixed(2)} A` : "-- A";
        document.getElementById("solarHealth").innerText  = (sv != null) ? (sv > 10 ? "Excellent" : sv > 5 ? "Good" : "Poor") : "--";

        // Alerts & Maintenance
        document.getElementById("alerts").innerText = "No active alerts";
        document.getElementById("maintenance").innerText = "30 days";
        document.getElementById("diagnostics").innerText = "System OK";
    } catch (err) {
        console.error("Failed to fetch metrics:", err);
        document.getElementById("batteryStatus").innerText = "--";
        document.getElementById("batteryVoltage").innerText = "-- V";
        document.getElementById("batteryLevel").innerText = "-- %";
        document.getElementById("timeToFull").innerText = "Time to Full: --";
        document.getElementById("batteryTemp").innerText = "-- °C";
        document.getElementById("powerOutput").innerText = "-- W";
        document.getElementById("energyConsumption").innerText = "-- kWh";
        document.getElementById("systemTemp").innerText = "-- °C";
        document.getElementById("solarEnergy").innerText = "-- kWh";
        document.getElementById("solarVoltage").innerText = "-- V";
        document.getElementById("solarCurrent").innerText = "-- A";
        document.getElementById("solarHealth").innerText = "--";
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