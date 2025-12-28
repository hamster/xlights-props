// HTML page for configuration
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Servo Controller</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ccircle cx='50' cy='50' r='15' fill='%23333'/%3E%3Cellipse cx='45' cy='45' rx='3' ry='4' fill='%23ff0000'/%3E%3Cellipse cx='55' cy='45' rx='3' ry='4' fill='%23ff0000'/%3E%3Cline x1='35' y1='40' x2='15' y2='25' stroke='%23333' stroke-width='3'/%3E%3Cline x1='30' y1='50' x2='5' y2='45' stroke='%23333' stroke-width='3'/%3E%3Cline x1='32' y1='60' x2='10' y2='70' stroke='%23333' stroke-width='3'/%3E%3Cline x1='35' y1='68' x2='15' y2='85' stroke='%23333' stroke-width='3'/%3E%3Cline x1='65' y1='40' x2='85' y2='25' stroke='%23333' stroke-width='3'/%3E%3Cline x1='70' y1='50' x2='95' y2='45' stroke='%23333' stroke-width='3'/%3E%3Cline x1='68' y1='60' x2='90' y2='70' stroke='%23333' stroke-width='3'/%3E%3Cline x1='65' y1='68' x2='85' y2='85' stroke='%23333' stroke-width='3'/%3E%3C/svg%3E">
  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 600px;
      margin: 50px auto;
      padding: 20px;
      background-color: #f0f0f0;
    }
    .container {
      background-color: white;
      padding: 30px;
      border-radius: 10px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    /* Desktop responsive layout */
    @media (min-width: 1024px) {
      body {
        max-width: 1400px;
      }
      .status-grid {
        display: grid;
        grid-template-columns: repeat(4, 1fr);
        gap: 20px;
      }
      .status-grid .status-box {
        margin: 0;
      }
      .settings-grid {
        display: grid;
        grid-template-columns: repeat(4, 1fr);
        gap: 20px;
        align-items: start;
      }
      .settings-grid > * {
        margin: 0;
      }
    }
    h1 {
      color: #333;
      text-align: center;
      margin: 0;
    }
    h3 {
      color: #555;
      margin-top: 0;
      margin-bottom: 15px;
    }
    .tabs {
      display: flex;
      gap: 5px;
      margin-bottom: 0;
      border-bottom: 2px solid #007bff;
    }
    .tab {
      flex: 1;
      padding: 12px 20px;
      text-align: center;
      cursor: pointer;
      background-color: #e9ecef;
      border: 2px solid #ddd;
      border-bottom: none;
      border-radius: 8px 8px 0 0;
      font-size: 16px;
      font-weight: bold;
      color: #666;
      transition: all 0.3s;
      margin-bottom: -2px;
    }
    .tab:hover {
      background-color: #dee2e6;
      color: #333;
    }
    .tab.active {
      background-color: white;
      color: #007bff;
      border: 2px solid #007bff;
      border-bottom: 2px solid white;
      position: relative;
      z-index: 1;
    }
    .tab-content {
      display: none;
    }
    .tab-content.active {
      display: block;
    }
    .status-box {
      padding: 15px;
      margin: 15px 0;
      border-radius: 5px;
      background-color: #f8f9fa;
      border-left: 4px solid #007bff;
    }
    .status-box h4 {
      margin-top: 0;
      color: #333;
    }
    .status-box p {
      margin: 8px 0;
      color: #555;
    }
    .status-box strong {
      color: #333;
    }
    .status {
      padding: 10px;
      margin: 20px 0;
      border-radius: 5px;
      text-align: center;
      font-weight: bold;
    }
    .connected {
      background-color: #d4edda;
      color: #155724;
    }
    .disconnected {
      background-color: #f8d7da;
      color: #721c24;
    }
    .homed {
      background-color: #d4edda;
      color: #155724;
    }
    .not-homed {
      background-color: #fff3cd;
      color: #856404;
    }
    input[type="text"], input[type="password"], input[type="number"] {
      width: 100%;
      padding: 12px;
      margin: 8px 0;
      box-sizing: border-box;
      border: 2px solid #ddd;
      border-radius: 4px;
    }
    input[type="checkbox"] {
      width: 20px;
      height: 20px;
      margin-right: 10px;
      vertical-align: middle;
    }
    .form-group {
      margin-bottom: 15px;
    }
    .button-row {
      display: flex;
      gap: 10px;
      margin: 10px 0;
    }
    .button-row button {
      flex: 1;
    }
    .collapsible {
      background-color: #f0f0f0;
      color: #333;
      cursor: pointer;
      padding: 12px;
      width: 100%;
      border: none;
      text-align: left;
      outline: none;
      font-size: 15px;
      font-weight: bold;
      border-radius: 4px;
      margin-top: 15px;
      transition: background-color 0.3s;
    }
    .collapsible:hover {
      background-color: #e0e0e0;
    }
    .collapsible:after {
      content: '\25BC';
      float: right;
      margin-left: 5px;
      font-size: 12px;
    }
    .collapsible.active:after {
      content: '\25B2';
    }
    .collapsible-content {
      max-height: 0;
      overflow: hidden;
      transition: max-height 0.3s ease-out;
      background-color: #f8f9fa;
      border-radius: 4px;
      margin-top: 5px;
    }
    .collapsible-content-inner {
      padding: 15px;
    }
    .notification {
      position: fixed;
      top: 20px;
      right: 20px;
      padding: 15px 20px;
      border-radius: 4px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.3);
      z-index: 1000;
      font-weight: bold;
      animation: slideIn 0.3s ease-out;
    }
    .notification.success {
      background-color: #28a745;
      color: white;
    }
    .notification.error {
      background-color: #dc3545;
      color: white;
    }
    @keyframes slideIn {
      from {
        transform: translateX(400px);
        opacity: 0;
      }
      to {
        transform: translateX(0);
        opacity: 1;
      }
    }
    button {
      width: 100%;
      padding: 12px;
      margin: 10px 0;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      font-size: 16px;
      font-weight: bold;
    }
    .btn-primary {
      background-color: #007bff;
      color: white;
    }
    .btn-primary:hover {
      background-color: #0056b3;
    }
    .btn-success {
      background-color: #28a745;
      color: white;
    }
    .btn-success:hover {
      background-color: #218838;
    }
    .btn-danger {
      background-color: #dc3545;
      color: white;
    }
    .btn-danger:hover {
      background-color: #c82333;
    }
    .btn-warning {
      background-color: #ffc107;
      color: #333;
    }
    .btn-warning:hover {
      background-color: #e0a800;
    }
    label {
      font-weight: bold;
      color: #555;
    }
    .header-container {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 15px;
      margin-bottom: 20px;
    }
    .locate-indicator {
      width: 40px;
      height: 40px;
      cursor: pointer;
      transition: transform 0.3s;
    }
    .locate-indicator:hover {
      transform: scale(1.15);
    }
    .locate-indicator.active {
      animation: spiderDance 0.3s infinite alternate;
    }
    .locate-indicator svg {
      width: 100%;
      height: 100%;
    }
    .locate-indicator .spider-body {
      transition: fill 0.3s;
    }
    .locate-indicator .spider-eyes {
      transition: fill 0.3s;
    }
    .locate-indicator.active .spider-body {
      animation: colorFlash 0.5s infinite alternate;
    }
    .locate-indicator.active .spider-eyes {
      animation: eyeFlash 0.5s infinite alternate;
    }
    @keyframes spiderDance {
      0% {
        transform: rotate(-5deg) translateY(-2px);
      }
      100% {
        transform: rotate(5deg) translateY(2px);
      }
    }
    @keyframes colorFlash {
      0% {
        fill: #dc3545;
      }
      100% {
        fill: #28a745;
      }
    }
    @keyframes eyeFlash {
      0% {
        fill: #ff0000;
      }
      100% {
        fill: #00ff00;
      }
    }
  </style>
  <script>
    function showNotification(message, isSuccess) {
      var notification = document.createElement('div');
      notification.className = 'notification ' + (isSuccess ? 'success' : 'error');
      notification.textContent = message;
      document.body.appendChild(notification);

      setTimeout(function() {
        notification.remove();
      }, 3000);
    }

    function handleFormSubmit(event, url) {
      event.preventDefault();
      var formData = new FormData(event.target);

      fetch(url, {
        method: 'POST',
        body: formData
      })
      .then(response => response.json())
      .then(data => {
        if (data.success) {
          showNotification(data.message, true);
        } else {
          showNotification(data.message || 'Error saving settings', false);
        }
      })
      .catch(error => {
        showNotification('Error: ' + error.message, false);
      });

      return false;
    }

    function showTab(tabName) {
      var i, tabcontent, tablinks;
      tabcontent = document.getElementsByClassName("tab-content");
      for (i = 0; i < tabcontent.length; i++) {
        tabcontent[i].classList.remove("active");
      }
      tablinks = document.getElementsByClassName("tab");
      for (i = 0; i < tablinks.length; i++) {
        tablinks[i].classList.remove("active");
      }
      document.getElementById(tabName).classList.add("active");
      event.target.classList.add("active");

      // Disable AJAX status updates on OTA update page to avoid interference
      if (tabName === 'update-tab') {
        if (window.statusUpdateInterval) {
          clearInterval(window.statusUpdateInterval);
          window.statusUpdateInterval = null;
          console.log('Status updates paused for OTA update page');
        }
      } else {
        // Resume status updates when leaving OTA page
        if (!window.statusUpdateInterval) {
          window.statusUpdateInterval = setInterval(function() {
            updateStatus();
          }, 1000);
          console.log('Status updates resumed');
        }
      }
    }

    function toggleCollapsible(element) {
      element.classList.toggle("active");
      var content = element.nextElementSibling;
      if (content.style.maxHeight) {
        content.style.maxHeight = null;
      } else {
        content.style.maxHeight = content.scrollHeight + "px";
      }
    }

    function toggleApPassword() {
      var passwordField = document.getElementById("apPassword");
      var checkbox = document.getElementById("showApPassword");
      passwordField.type = checkbox.checked ? "text" : "password";
    }

    function toggleStaticIpFields() {
      var staticIpFields = document.getElementById("staticIpFields");
      var useStatic = document.getElementById("useStatic");
      staticIpFields.style.display = useStatic.checked ? "block" : "none";
    }

    // Initialize static IP fields visibility on page load
    window.addEventListener('load', function() {
      toggleStaticIpFields();
    });

    function moveStepsStatus(direction) {
      var steps = document.getElementById("stepAmountStatus").value;
      var position = direction === 'forward' ? steps : -steps;
      fetch('/move?steps=' + position)
        .then(response => response.text())
        .then(data => {
          console.log('Moved ' + position + ' steps');
        });
    }

    function setPositionStatus() {
      var position = document.getElementById("setPositionInputStatus").value;
      fetch('/set-position?position=' + position)
        .then(response => response.text())
        .then(data => {
          console.log('Moving to position ' + position);
        });
    }

    function setPercentPosition() {
      var percent = document.getElementById("setPercentInput").value;
      var bottomPos = parseInt(document.getElementById('bottom-position').textContent);
      var position = Math.round((bottomPos * percent) / 100);
      fetch('/set-position?position=' + position)
        .then(response => response.text())
        .then(data => {
          console.log('Moving to ' + percent + '% (position ' + position + ')');
        });
    }

    function moveSteps(direction) {
      var steps = document.getElementById("stepAmount").value;
      var position = direction === 'forward' ? steps : -steps;
      fetch('/move?steps=' + position)
        .then(response => response.text())
        .then(data => {
          console.log('Moved ' + position + ' steps');
        });
    }

    function setPosition() {
      var position = document.getElementById("setPositionInput").value;
      fetch('/set-position?position=' + position)
        .then(response => response.text())
        .then(data => {
          console.log('Moving to position ' + position);
        });
    }

    function homeServo() {
      fetch('/home')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            alert(data.message);
          } else {
            alert('Homing failed: ' + data.message);
          }
        })
        .catch(error => {
          alert('Error starting homing: ' + error);
        });
    }

    var locateModeActive = false;
    function toggleLocate() {
      locateModeActive = !locateModeActive;
      var indicator = document.getElementById('locate-indicator');

      fetch('/locate?enable=' + locateModeActive)
        .then(response => response.json())
        .then(data => {
          if (data.locateMode) {
            indicator.classList.add('active');
            console.log('Locate mode enabled - LED showing SOS');
          } else {
            indicator.classList.remove('active');
            console.log('Locate mode disabled');
          }
        })
        .catch(error => {
          console.log('Error toggling locate mode: ' + error);
        });
    }

    var isOffline = false;
    var offlineNotification = null;
    var reconnectInterval = null;

    function showOfflineNotification() {
      if (!offlineNotification) {
        offlineNotification = document.createElement('div');
        offlineNotification.className = 'notification error';
        offlineNotification.textContent = 'Controller is offline or not reachable';
        offlineNotification.style.display = 'block';
        document.body.appendChild(offlineNotification);
      }
    }

    function hideOfflineNotification() {
      if (offlineNotification) {
        offlineNotification.remove();
        offlineNotification = null;
      }
    }

    function updateStatus() {
      fetch('/status-data', { timeout: 5000 })
        .then(response => {
          if (!response.ok) {
            throw new Error('Network response was not ok');
          }
          return response.json();
        })
        .then(data => {
          // Connection successful - clear offline state
          if (isOffline) {
            isOffline = false;
            hideOfflineNotification();
            if (reconnectInterval) {
              clearInterval(reconnectInterval);
              reconnectInterval = null;
            }
          }

          // Update WiFi status
          document.getElementById('wifi-status-text').textContent = data.wifiConnected ? 'Connected' : 'Not Connected';
          document.getElementById('wifi-status-text').className = 'status ' + (data.wifiConnected ? 'connected' : 'disconnected');
          document.getElementById('wifi-mode').textContent = data.wifiMode;
          document.getElementById('wifi-network').textContent = data.wifiNetwork;
          document.getElementById('ip-type').textContent = data.ipType;
          document.getElementById('wifi-ip').textContent = data.wifiIp;
          document.getElementById('wifi-subnet').textContent = data.wifiSubnet;
          document.getElementById('wifi-gateway').textContent = data.wifiGateway;

          // Update Uptime
          var uptimeStr = data.uptimeDays + 'd ' + data.uptimeHours + 'h ' + data.uptimeMins + 'm ' + data.uptimeSecs + 's';
          document.getElementById('uptime').textContent = uptimeStr;

          // Update Stepper status
          if (data.isHoming) {
            document.getElementById('homed-status-text').textContent = 'Homing...';
            document.getElementById('homed-status-text').className = 'status not-homed';
          } else {
            document.getElementById('homed-status-text').textContent = data.homed ? 'Homed' : 'Not Homed';
            document.getElementById('homed-status-text').className = 'status ' + (data.homed ? 'homed' : 'not-homed');
          }

          // Update homing switch status
          var homingSwitchElement = document.getElementById('homing-switch-status');
          if (data.homingSwitchTripped) {
            homingSwitchElement.textContent = 'Tripped';
            homingSwitchElement.className = 'status not-homed';
          } else {
            homingSwitchElement.textContent = 'Not Tripped';
            homingSwitchElement.className = 'status homed';
          }

          document.getElementById('current-position').textContent = data.position;
          document.getElementById('position-percent').textContent = data.positionPercent;
          document.getElementById('bottom-position').textContent = data.bottomPosition;

          // Update ArtNet status
          var artnetStatusElement = document.getElementById('artnet-processing-status');
          if (!data.artnetEnabled) {
            artnetStatusElement.textContent = 'ArtNet Disabled';
            artnetStatusElement.className = 'status disconnected';
          } else if (!data.homed) {
            artnetStatusElement.textContent = 'ArtNet Enabled - Not Homed';
            artnetStatusElement.className = 'status not-homed';
          } else {
            artnetStatusElement.textContent = 'ArtNet Enabled';
            artnetStatusElement.className = 'status connected';
          }
          document.getElementById('artnet-universe').textContent = data.artnetUniverse;
          document.getElementById('artnet-channel').textContent = data.artnetChannel;
          document.getElementById('artnet-packets-received').textContent = data.artnetPacketsReceived;
          document.getElementById('artnet-packets-acted').textContent = data.artnetPacketsActedOn;
          document.getElementById('artnet-last-command').textContent = data.artnetLastCommand;
          document.getElementById('artnet-last-command-percent').textContent = data.artnetLastCommandPercent;

          // Update DDP status
          var ddpStatusElement = document.getElementById('ddp-processing-status');
          if (!data.ddpEnabled) {
            ddpStatusElement.textContent = 'DDP Disabled';
            ddpStatusElement.className = 'status disconnected';
          } else if (!data.homed) {
            ddpStatusElement.textContent = 'DDP Enabled - Not Homed';
            ddpStatusElement.className = 'status not-homed';
          } else {
            ddpStatusElement.textContent = 'DDP Enabled';
            ddpStatusElement.className = 'status connected';
          }
          document.getElementById('ddp-servo-channel').textContent = data.ddpServoChannel;
          document.getElementById('ddp-packets-received').textContent = data.ddpPacketsReceived;
          document.getElementById('ddp-packets-acted').textContent = data.ddpPacketsActedOn;
          document.getElementById('ddp-last-command').textContent = data.ddpLastCommand;

          // Update locate mode indicator to sync across all clients
          var indicator = document.getElementById('locate-indicator');
          if (data.locateMode) {
            if (!indicator.classList.contains('active')) {
              indicator.classList.add('active');
              locateModeActive = true;
            }
          } else {
            if (indicator.classList.contains('active')) {
              indicator.classList.remove('active');
              locateModeActive = false;
            }
          }
        })
        .catch(error => {
          // Connection failed - set offline state
          if (!isOffline) {
            isOffline = true;
            showOfflineNotification();

            // Start reconnection attempts every 30 seconds
            if (!reconnectInterval) {
              reconnectInterval = setInterval(updateStatus, 30000);
            }
          }
          console.log('Controller offline or unreachable:', error);
        });
    }

    // Update status every second
    // Always update to keep locate mode synced across all clients
    // Store interval ID globally so it can be paused during OTA updates
    window.statusUpdateInterval = setInterval(function() {
      updateStatus();
    }, 1000);
  </script>
</head>
<body>
  <div class="container">
    <div class="header-container">
      <h1>Servo Controller</h1>
      <div id="locate-indicator" class="locate-indicator" onclick="toggleLocate()" title="Click to locate this device (LED will blink SOS)">
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
          <circle cx="50" cy="50" r="15" class="spider-body" fill="#333"/>
          <ellipse cx="45" cy="45" rx="3" ry="4" class="spider-eyes" fill="#ff0000"/>
          <ellipse cx="55" cy="45" rx="3" ry="4" class="spider-eyes" fill="#ff0000"/>
          <line x1="35" y1="40" x2="15" y2="25" stroke="#333" stroke-width="3"/>
          <line x1="30" y1="50" x2="5" y2="45" stroke="#333" stroke-width="3"/>
          <line x1="32" y1="60" x2="10" y2="70" stroke="#333" stroke-width="3"/>
          <line x1="35" y1="68" x2="15" y2="85" stroke="#333" stroke-width="3"/>
          <line x1="65" y1="40" x2="85" y2="25" stroke="#333" stroke-width="3"/>
          <line x1="70" y1="50" x2="95" y2="45" stroke="#333" stroke-width="3"/>
          <line x1="68" y1="60" x2="90" y2="70" stroke="#333" stroke-width="3"/>
          <line x1="65" y1="68" x2="85" y2="85" stroke="#333" stroke-width="3"/>
        </svg>
      </div>
    </div>

    <div class="tabs">
      <button class="tab active" onclick="showTab('status-tab')">Status</button>
      <button class="tab" onclick="showTab('settings-tab')">Settings</button>
      <button class="tab" onclick="showTab('update-tab')">OTA Update</button>
    </div>

    <!-- Status Tab -->
    <div id="status-tab" class="tab-content active">
      <div class="status-grid">
      <div class="status-box">
        <h4>WiFi Status</h4>
        <div id="wifi-status-text" class="status {{STATUS_CLASS}}">
          {{STATUS_TEXT}}
        </div>
        <p><strong>Mode:</strong> <span id="wifi-mode">{{WIFI_MODE}}</span></p>
        <p><strong>Network:</strong> <span id="wifi-network">{{WIFI_NETWORK}}</span></p>
        <p><strong>IP Type:</strong> <span id="ip-type">N/A</span></p>
        <p><strong>IP Address:</strong> <span id="wifi-ip">{{WIFI_IP}}</span></p>
        <p><strong>Subnet Mask:</strong> <span id="wifi-subnet">N/A</span></p>
        <p><strong>Gateway:</strong> <span id="wifi-gateway">N/A</span></p>
        <p><strong>Uptime:</strong> <span id="uptime">0d 0h 0m 0s</span></p>
      </div>

      <div class="status-box">
        <h4>Stepper Status</h4>
        <div id="homed-status-text" class="status {{HOMED_CLASS}}">
          {{HOMED_TEXT}}
        </div>
        <p><strong>Homing Switch:</strong> <span id="homing-switch-status" class="status">Not Tripped</span></p>
        <p><strong>Current Position:</strong> <span id="current-position">{{CURRENT_POSITION}}</span> steps</p>
        <p><strong>Position (%):</strong> <span id="position-percent">{{POSITION_PERCENT}}</span>%</p>
        <p><strong>Bottom Position:</strong> <span id="bottom-position">{{BOTTOM_POSITION}}</span> steps</p>
        <p><strong>Auto Home on Boot:</strong> <span id="auto-home-on-boot">{{AUTO_HOME_STATUS}}</span></p>

        <button class="collapsible" onclick="toggleCollapsible(this)">Manual Stepper Control</button>
        <div class="collapsible-content">
          <div class="collapsible-content-inner">
            <h4 style="margin-top: 0;">Position Control</h4>

            <div class="form-group">
              <label for="setPositionInputStatus">Move to Position (steps):</label>
              <input type="number" id="setPositionInputStatus" name="setPositionInputStatus" value="0" min="0" max="100000">
              <button onclick="setPositionStatus()" class="btn-warning">Go to Position</button>
            </div>

            <div class="form-group">
              <label for="setPercentInput">Move to Position (%):</label>
              <input type="number" id="setPercentInput" name="setPercentInput" value="0" min="0" max="100">
              <button onclick="setPercentPosition()" class="btn-warning">Go to Percent</button>
            </div>

            <h4>Incremental Movement</h4>

            <div class="form-group">
              <label for="stepAmountStatus">Step Amount:</label>
              <input type="number" id="stepAmountStatus" name="stepAmountStatus" value="50" min="1" max="10000">
            </div>

            <div class="button-row">
              <button onclick="moveStepsStatus('backward')" class="btn-warning">&larr; Move Backward</button>
              <button onclick="moveStepsStatus('forward')" class="btn-warning">Move Forward &rarr;</button>
            </div>

            <button onclick="homeServo()" class="btn-success">Home Servo</button>
          </div>
        </div>
      </div>

      <div class="status-box">
        <h4>ArtNet Status</h4>
        <div id="artnet-processing-status" class="status not-homed" style="margin-bottom: 10px;">
          ArtNet Disabled - Not Homed
        </div>
        <p><strong>Universe:</strong> <span id="artnet-universe">{{ARTNET_UNIVERSE}}</span></p>
        <p><strong>Channel:</strong> <span id="artnet-channel">{{ARTNET_CHANNEL}}</span></p>
        <p><strong>Packets Received:</strong> <span id="artnet-packets-received">0</span></p>
        <p><strong>Packets for this Channel:</strong> <span id="artnet-packets-acted">0</span></p>
        <p><strong>Last Command:</strong> <span id="artnet-last-command">0</span> (<span id="artnet-last-command-percent">0.0</span>%)</p>
      </div>

      <div class="status-box">
        <h4>DDP Status</h4>
        <div id="ddp-processing-status" class="status not-homed" style="margin-bottom: 10px;">
          DDP Disabled - Not Homed
        </div>
        <p><strong>Servo Channel:</strong> <span id="ddp-servo-channel">{{DDP_SERVO_CHANNEL}}</span></p>
        <p><strong>Packets Received:</strong> <span id="ddp-packets-received">0</span></p>
        <p><strong>Packets for this Channel:</strong> <span id="ddp-packets-acted">0</span></p>
        <p><strong>Last Command:</strong> <span id="ddp-last-command">0</span></p>
      </div>
      </div>
    </div>

    <!-- Settings Tab -->
    <div id="settings-tab" class="tab-content">
      <div class="settings-grid">
      <!-- WiFi & Access Point Configuration Box -->
      <div class="status-box">
        <h4>WiFi & Access Point Configuration</h4>

        <form onsubmit="return handleFormSubmit(event, '/save-wifi')">
          <h3>WiFi Client Settings</h3>
          <div class="form-group">
            <label for="ssid">WiFi Network (SSID):</label>
            <input type="text" id="ssid" name="ssid" value="{{CURRENT_SSID}}" required>
          </div>

          <div class="form-group">
            <label for="password">Password:</label>
            <input type="password" id="password" name="password" placeholder="Enter WiFi password">
          </div>

          <div class="form-group">
            <label style="display: block; margin-bottom: 8px;">
              <input type="radio" id="useDhcp" name="ipMode" value="dhcp" {{DHCP_CHECKED}} onchange="toggleStaticIpFields()">
              Use DHCP (Automatic)
            </label>
            <label style="display: block;">
              <input type="radio" id="useStatic" name="ipMode" value="static" {{STATIC_CHECKED}} onchange="toggleStaticIpFields()">
              Use Static IP
            </label>
          </div>

          <div id="staticIpFields" style="display: none;">
            <div class="form-group">
              <label for="staticIp">IP Address:</label>
              <input type="text" id="staticIp" name="staticIp" value="{{STATIC_IP}}" placeholder="192.168.1.100">
            </div>

            <div class="form-group">
              <label for="staticGateway">Gateway:</label>
              <input type="text" id="staticGateway" name="staticGateway" value="{{STATIC_GATEWAY}}" placeholder="192.168.1.1">
            </div>

            <div class="form-group">
              <label for="staticSubnet">Subnet Mask:</label>
              <input type="text" id="staticSubnet" name="staticSubnet" value="{{STATIC_SUBNET}}" placeholder="255.255.255.0">
            </div>
          </div>

          <button type="submit" class="btn-primary">Save WiFi Settings</button>
        </form>

        <div style="margin: 10px 0;">
          <button onclick="location.href='/connect'" class="btn-success">Connect Now</button>
        </div>

        <hr style="margin: 30px 0; border: none; border-top: 1px solid #ddd;">

        <form onsubmit="return handleFormSubmit(event, '/save-ap')">
          <h3>Access Point Settings</h3>
          <div class="form-group">
            <label for="apSsid">AP Name:</label>
            <input type="text" id="apSsid" name="apSsid" value="{{AP_SSID}}" required>
          </div>

          <div class="form-group">
            <label for="apPassword">AP Password:</label>
            <input type="password" id="apPassword" name="apPassword" value="{{AP_PASSWORD}}" required minlength="8">
          </div>

          <div class="form-group">
            <label for="showApPassword">
              <input type="checkbox" id="showApPassword" onchange="toggleApPassword()">
              Show Password
            </label>
          </div>

          <div class="form-group">
            <label for="apAppendMac">
              <input type="checkbox" id="apAppendMac" name="apAppendMac" {{AP_APPEND_MAC_CHECKED}}>
              Append MAC Address (last 4 digits)
            </label>
          </div>

          <button type="submit" class="btn-primary">Save AP Settings</button>
        </form>
      </div>

      <!-- Stepper Configuration Box -->
      <div class="status-box">
        <h4>Stepper Configuration</h4>

        <form onsubmit="return handleFormSubmit(event, '/save-stepper')">
          <div class="form-group">
            <label for="stepperSpeed">Stepper Speed (Hz):</label>
            <input type="number" id="stepperSpeed" name="stepperSpeed" value="{{STEPPER_SPEED}}" min="100" max="50000" required>
          </div>

          <div class="form-group">
            <label for="stepperAccel">Stepper Acceleration (steps/s&sup2;):</label>
            <input type="number" id="stepperAccel" name="stepperAccel" value="{{STEPPER_ACCEL}}" min="0" max="1000000" required>
          </div>

          <div class="form-group">
            <label for="jumpStart">Jump Start (steps):</label>
            <input type="number" id="jumpStart" name="jumpStart" value="{{JUMP_START}}" min="0" max="10000" required>
          </div>

          <div class="form-group">
            <label for="autoHomeOnBoot">
              <input type="checkbox" id="autoHomeOnBoot" name="autoHomeOnBoot" {{AUTO_HOME_ON_BOOT_CHECKED}}>
              Auto Home on Bootup
            </label>
          </div>

          <button type="submit" class="btn-primary">Save Stepper Settings</button>
        </form>

        <hr style="margin: 30px 0; border: none; border-top: 1px solid #ddd;">

        <h3>Stepper Control</h3>

        <div class="form-group">
          <label for="setPositionInput">Set Position (steps):</label>
          <input type="number" id="setPositionInput" name="setPositionInput" value="0" min="0" max="100000">
          <button onclick="setPosition()" class="btn-warning">Go to Position</button>
        </div>

        <div class="form-group">
          <label for="stepAmount">Step Amount:</label>
          <input type="number" id="stepAmount" name="stepAmount" value="50" min="1" max="10000">
        </div>

        <div class="button-row">
          <button onclick="moveSteps('backward')" class="btn-warning">&larr; Move Backward</button>
          <button onclick="moveSteps('forward')" class="btn-warning">Move Forward &rarr;</button>
        </div>

        <button onclick="homeServo()" class="btn-success">Home Servo</button>
      </div>

      <!-- ArtNet Configuration Box -->
      <div class="status-box">
        <h4>ArtNet Configuration</h4>

        <form onsubmit="return handleFormSubmit(event, '/save-artnet')">
          <div class="form-group">
            <label for="artnetEnabled">
              <input type="checkbox" id="artnetEnabled" name="artnetEnabled" {{ARTNET_ENABLED_CHECKED}}>
              Enable ArtNet Control
            </label>
          </div>

          <div class="form-group">
            <label for="artnetUniverse">ArtNet Universe:</label>
            <input type="number" id="artnetUniverse" name="artnetUniverse" value="{{ARTNET_UNIVERSE}}" min="0" max="32767" required>
          </div>

          <div class="form-group">
            <label for="artnetChannel">DMX Channel (1-512):</label>
            <input type="number" id="artnetChannel" name="artnetChannel" value="{{ARTNET_CHANNEL}}" min="1" max="512" required>
          </div>

          <div class="form-group">
            <label for="artnetDebug">
              <input type="checkbox" id="artnetDebug" name="artnetDebug" {{ARTNET_DEBUG_CHECKED}}>
              Enable Serial Debug Output
            </label>
          </div>

          <button type="submit" class="btn-primary">Save ArtNet Settings</button>
        </form>
      </div>

      <!-- DDP Configuration Box -->
      <div class="status-box">
        <h4>DDP Configuration</h4>

        <form onsubmit="return handleFormSubmit(event, '/save-ddp')">
          <div class="form-group">
            <label for="ddpEnabled">
              <input type="checkbox" id="ddpEnabled" name="ddpEnabled" {{DDP_ENABLED_CHECKED}}>
              Enable DDP Control
            </label>
          </div>

          <div class="form-group">
            <label for="ddpServoChannel">Servo Channel (1-512):</label>
            <input type="number" id="ddpServoChannel" name="ddpServoChannel" value="{{DDP_SERVO_CHANNEL}}" min="1" max="512" required>
          </div>

          <div class="form-group">
            <label for="ddpDebug">
              <input type="checkbox" id="ddpDebug" name="ddpDebug" {{DDP_DEBUG_CHECKED}}>
              Enable Serial Debug Output
            </label>
          </div>

          <button type="submit" class="btn-primary">Save DDP Settings</button>
        </form>
      </div>
      </div>

      <button onclick="if(confirm('Reboot device?')) location.href='/reboot'" style="margin-top: 20px; width: 100%;" class="btn-danger">Reboot Device</button>
    </div>

    <!-- OTA Update Tab -->
    <div id="update-tab" class="tab-content">
      <div class="status-box" style="max-width: 600px; margin: 0 auto;">
        <h4>Firmware Update (OTA)</h4>
        <p style="color: #666; margin-bottom: 20px;">
          Upload a new firmware.bin file to update the device. The device will automatically reboot after a successful update.
        </p>

        <div style="background-color: #fff3cd; border: 1px solid #ffc107; border-radius: 4px; padding: 15px; margin-bottom: 20px;">
          <strong style="color: #856404;">&#9888; Warning:</strong>
          <ul style="margin: 10px 0 0 20px; color: #856404;">
            <li>Do not disconnect power during the update</li>
            <li>Make sure you are uploading a valid firmware.bin file</li>
            <li>The update process may take up to 2 minutes</li>
            <li>The device will reboot automatically when complete</li>
          </ul>
        </div>

        <form id="otaForm" method="POST" action="/update" enctype="multipart/form-data">
          <div class="form-group">
            <label for="firmwareFile">Select Firmware File (.bin):</label>
            <input type="file" id="firmwareFile" name="firmware" accept=".bin" required style="width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; border: 2px solid #ddd; border-radius: 4px;">
          </div>

          <button type="submit" class="btn-primary" id="updateButton">Upload Firmware</button>
        </form>

        <div id="updateProgress" style="display: none; margin-top: 20px;">
          <div style="background-color: #f0f0f0; border-radius: 4px; height: 30px; overflow: hidden;">
            <div id="progressBar" style="background-color: #007bff; height: 100%; width: 0%; transition: width 0.3s; display: flex; align-items: center; justify-content: center; color: white; font-weight: bold;">
              <span id="progressText">0%</span>
            </div>
          </div>
          <p id="updateStatus" style="text-align: center; margin-top: 10px; font-weight: bold;"></p>
        </div>
      </div>
    </div>

    <p style="text-align: center; color: #888; font-size: 12px; margin-top: 20px; padding-bottom: 10px;">
      Version {{VERSION}} | Built: {{BUILD_DATE}} {{BUILD_TIME}}
    </p>
  </div>
  <script>
    // OTA Update JavaScript
    document.getElementById('otaForm').addEventListener('submit', function(e) {
      e.preventDefault();

      var fileInput = document.getElementById('firmwareFile');
      if (fileInput.files.length === 0) {
        alert('Please select a firmware file');
        return;
      }

      var file = fileInput.files[0];
      if (!file.name.endsWith('.bin')) {
        alert('Please select a valid .bin file');
        return;
      }

      // Show progress
      document.getElementById('updateProgress').style.display = 'block';
      document.getElementById('updateButton').disabled = true;
      document.getElementById('updateStatus').textContent = 'Uploading firmware...';

      var formData = new FormData();
      formData.append('firmware', file);

      var xhr = new XMLHttpRequest();

      // Progress handler
      xhr.upload.addEventListener('progress', function(e) {
        if (e.lengthComputable) {
          var percentComplete = Math.round((e.loaded / e.total) * 100);
          document.getElementById('progressBar').style.width = percentComplete + '%';
          document.getElementById('progressText').textContent = percentComplete + '%';
        }
      });

      // Completion handler
      xhr.addEventListener('load', function() {
        if (xhr.status === 200) {
          document.getElementById('progressBar').style.width = '100%';
          document.getElementById('progressText').textContent = '100%';
          document.getElementById('progressBar').style.backgroundColor = '#28a745';
          document.getElementById('updateStatus').textContent = 'Update successful! Device is rebooting...';
          document.getElementById('updateStatus').style.color = '#28a745';

          // Redirect to home page after 5 seconds
          setTimeout(function() {
            window.location.href = '/';
          }, 5000);
        } else {
          document.getElementById('progressBar').style.backgroundColor = '#dc3545';
          document.getElementById('updateStatus').textContent = 'Update failed: ' + xhr.responseText;
          document.getElementById('updateStatus').style.color = '#dc3545';
          document.getElementById('updateButton').disabled = false;
        }
      });

      // Error handler
      xhr.addEventListener('error', function() {
        document.getElementById('progressBar').style.backgroundColor = '#dc3545';
        document.getElementById('updateStatus').textContent = 'Upload error - please try again';
        document.getElementById('updateStatus').style.color = '#dc3545';
        document.getElementById('updateButton').disabled = false;
      });

      xhr.open('POST', '/update');
      xhr.send(formData);
    });
  </script>
</body>
</html>
)rawliteral";
