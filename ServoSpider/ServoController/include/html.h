// HTML page for configuration
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <meta http-equiv="Pragma" content="no-cache">
  <meta http-equiv="Expires" content="0">
  <title>Servo Controller</title>
  <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ccircle cx='50' cy='50' r='15' fill='%23333'/%3E%3Cellipse cx='45' cy='45' rx='3' ry='4' fill='%23ff0000'/%3E%3Cellipse cx='55' cy='45' rx='3' ry='4' fill='%23ff0000'/%3E%3Cline x1='35' y1='40' x2='15' y2='25' stroke='%23333' stroke-width='3'/%3E%3Cline x1='30' y1='50' x2='5' y2='45' stroke='%23333' stroke-width='3'/%3E%3Cline x1='32' y1='60' x2='10' y2='70' stroke='%23333' stroke-width='3'/%3E%3Cline x1='35' y1='68' x2='15' y2='85' stroke='%23333' stroke-width='3'/%3E%3Cline x1='65' y1='40' x2='85' y2='25' stroke='%23333' stroke-width='3'/%3E%3Cline x1='70' y1='50' x2='95' y2='45' stroke='%23333' stroke-width='3'/%3E%3Cline x1='68' y1='60' x2='90' y2='70' stroke='%23333' stroke-width='3'/%3E%3Cline x1='65' y1='68' x2='85' y2='85' stroke='%23333' stroke-width='3'/%3E%3C/svg%3E">
  <style>
body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 0;
      background: linear-gradient(135deg, #0f1929 0%, #1a2332 100%);
      min-height: 100vh;
      color: #e0e0e0;
    }
    .container {
      background-color: transparent;
      padding: 0;
      border-radius: 0;
      box-shadow: none;
      max-width: 100%;
    }
    /* Desktop responsive layout */
    @media (min-width: 1024px) {
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
    .content-wrapper {
      max-width: 1400px;
      margin: 0 auto;
      padding: 20px;
    }
    h1 {
      color: #ffffff;
      text-align: center;
      margin: 0;
    }
    h3 {
      color: #c0c0c0;
      margin-top: 0;
      margin-bottom: 15px;
    }
    .banner-header {
      width: 100%;
      background: linear-gradient(90deg, #1a1a2e 0%, #16213e 100%);
      border-bottom: 3px solid #00d4ff;
      padding: 0;
      margin: 0;
      position: relative;
      height: 100px;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .banner-content {
      display: flex;
      align-items: center;
      gap: 20px;
      max-width: 1400px;
      width: 100%;
      padding: 0 40px;
    }
    .banner-title {
      font-size: 32px;
      font-weight: bold;
      color: #ffffff;
      margin: 0;
      flex: 1;
    }
    .banner-hostname {
      font-size: 16px;
      color: #a0a0a0;
      font-weight: normal;
    }
    .banner-spider {
      cursor: pointer;
      display: inline-block;
      width: 60px;
      height: 60px;
    }
    .banner-spider svg {
      display: block;
      width: 100%;
      height: 100%;
      transition: transform 0.3s;
      transform-origin: center center;
    }
    .banner-spider:hover svg {
      transform: scale(1.15);
    }
    .banner-spider.active svg {
      animation: spiderDance 0.3s infinite alternate;
      transform-origin: center center;
    }
    .banner-spider .spider-body {
      transition: fill 0.3s;
    }
    .banner-spider .spider-head {
      transition: fill 0.3s;
    }
    .banner-spider .spider-eyes {
      transition: fill 0.3s;
    }
    .banner-spider.active .spider-body,
    .banner-spider.active .spider-head {
      animation: colorFlash 0.5s infinite alternate;
    }
    .banner-spider.active .spider-eyes {
      animation: eyeFlash 0.5s infinite alternate;
    }
    .banner-spider.active .spider-leg {
      animation: legFlash 0.5s infinite alternate;
    }
    @keyframes legFlash {
      0% {
        stroke: #dc3545;
      }
      100% {
        stroke: #28a745;
      }
    }
    .tabs {
      display: flex;
      gap: 0;
      margin-bottom: 0;
      background: #0a0f1a;
      padding: 0 20px;
      max-width: 1400px;
      margin: 0 auto;
    }
    .tab {
      padding: 18px 30px;
      text-align: center;
      cursor: pointer;
      background-color: #1a2332;
      border: none;
      border-radius: 0;
      font-size: 16px;
      font-weight: bold;
      color: #8a9ba8;
      transition: all 0.2s;
      border-right: 1px solid #0a0f1a;
      position: relative;
    }
    .tab:hover {
      background-color: #243447;
      color: #c0d0e0;
    }
    .tab.active {
      background-color: #1e2d3d;
      color: #00d4ff;
      border-bottom: 3px solid #00d4ff;
      position: relative;
    }
    .tab-content {
      display: none;
    }
    .tab-content.active {
      display: block;
    }
    .status-box {
      padding: 20px;
      margin: 15px 0;
      border-radius: 8px;
      background-color: #1a2332;
      border-left: 4px solid #00d4ff;
      box-shadow: 0 4px 6px rgba(0,0,0,0.3);
    }
    .status-box h4 {
      margin-top: 0;
      color: #ffffff;
      font-size: 18px;
    }
    .status-box p {
      margin: 8px 0;
      color: #b0c0d0;
    }
    .status-box strong {
      color: #e0e0e0;
    }
    .status {
      padding: 8px 10px;
      margin: 10px 0;
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
    .homing-error {
      background-color: #f8d7da;
      color: #721c24;
      font-weight: bold;
    }
    .option-disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    input[type="text"], input[type="password"], input[type="number"] {
      width: 100%;
      padding: 12px;
      margin: 8px 0;
      box-sizing: border-box;
      border: 2px solid #2a3a4a;
      border-radius: 4px;
      background-color: #0f1929;
      color: #e0e0e0;
    }
    input[type="text"]:focus, input[type="password"]:focus, input[type="number"]:focus {
      outline: none;
      border-color: #00d4ff;
      background-color: #152030;
    }
    select {
      background-color: #0f1929;
      color: #e0e0e0;
      border: 2px solid #2a3a4a;
    }
    select:focus {
      outline: none;
      border-color: #00d4ff;
      background-color: #152030;
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
    .password-wrapper {
      position: relative;
      display: inline-block;
      width: 100%;
    }
    .password-wrapper input {
      width: 100%;
      padding-right: 45px;
      margin: 8px 0;
    }
    .password-toggle {
      position: absolute;
      right: 12px;
      top: 8px;
      bottom: 8px;
      margin: auto;
      background: none;
      border: none;
      cursor: pointer;
      padding: 0;
      width: 24px;
      height: 24px;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #666;
      transition: color 0.2s;
    }
    .password-toggle:hover {
      color: #333;
    }
    .password-toggle svg {
      width: 20px;
      height: 20px;
      fill: currentColor;
    }
    .position-input-group {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .position-input-group input[type="number"] {
      width: 100%;
      margin: 0;
    }
    .position-controls {
      display: flex;
      gap: 10px;
      align-items: center;
    }
    .position-mode-selector {
      display: flex;
      flex-direction: column;
      gap: 5px;
      font-size: 14px;
    }
    .position-mode-selector label {
      display: flex;
      align-items: center;
      gap: 5px;
      cursor: pointer;
    }
    .position-mode-selector input[type="radio"] {
      width: auto;
      margin: 0;
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
      background-color: #243447;
      color: #e0e0e0;
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
      background-color: #2d4158;
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
      background-color: #1a2332;
      border-radius: 4px;
      margin-top: 5px;
    }
    .collapsible-content-inner {
      padding: 15px;
      color: #b0c0d0;
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
      color: #b0c0d0;
    }
    .header-container {
      display: none;
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
    hr {
      border: none;
      border-top: 1px solid #2a3a4a;
    }
    .led-preview-box {
      border: 1px solid #2a3a4a;
      background: #0f1929;
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

    function toggleStepperOptions() {
      var stepperControlChecked = document.getElementById('stepperControl').checked;
      document.getElementById('control16BitGroup').style.display = stepperControlChecked ? 'block' : 'none';
      document.getElementById('stepperBlankTimeGroup').style.display = stepperControlChecked ? 'block' : 'none';
    }

    function clearTmcStall() {
      fetch('/clear-tmc-stall')
        .then(response => response.json())
        .then(data => {
          showNotification('Stall fault cleared', true);
        })
        .catch(error => {
          showNotification('Error clearing stall: ' + error, false);
        });
    }

    // LED preview toggle
    var ledPreviewEnabled = false;
    function toggleLedPreview() {
      ledPreviewEnabled = document.getElementById('led-preview-enabled').checked;
      document.getElementById('led-preview-container').style.display = ledPreviewEnabled ? 'block' : 'none';
      if (!ledPreviewEnabled) {
        document.getElementById('led-preview').innerHTML = '';
      } else {
        // Fetch immediately when enabled
        fetchLedPreview();
      }
    }

    function toggleLedTestMode() {
      var enabled = document.getElementById('led-test-mode-enabled').checked;
      fetch('/led-test?enable=' + enabled)
        .then(response => response.json())
        .then(data => {
          console.log('LED test mode: ' + (data.ledTestMode ? 'enabled' : 'disabled'));
        })
        .catch(error => {
          showNotification('Error toggling LED test mode: ' + error, false);
        });
    }

    function fetchLedPreview() {
      fetch('/led-preview')
        .then(response => response.json())
        .then(data => {
          var previewContainer = document.getElementById('led-preview');
          // New compact format: hex string "RRGGBBRRGGBB..." (6 chars per pixel)
          if (data.ledPreview && data.ledPreview.length > 0) {
            var html = '';
            var hexStr = data.ledPreview;
            // Each pixel is 6 hex chars (RRGGBB)
            for (var i = 0; i < hexStr.length; i += 6) {
              var color = '#' + hexStr.substr(i, 6);
              html += '<div style="width:6px;height:6px;background:' + color + ';"></div>';
            }
            previewContainer.innerHTML = html;
          } else {
            previewContainer.innerHTML = '<span style="color:#666;font-size:12px;">No LED data</span>';
          }
        })
        .catch(error => {
          console.log('LED preview fetch error:', error);
        });
    }

    // Initialize stepper options visibility on page load
    document.addEventListener('DOMContentLoaded', function() {
      toggleStepperOptions();
    });

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

    function togglePassword(fieldId, buttonId) {
      var passwordField = document.getElementById(fieldId);
      var toggleButton = document.getElementById(buttonId);

      // Eye open icon (showing password)
      var eyeOpen = '<svg viewBox="0 0 24 24"><path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg>';

      // Eye closed icon (hiding password)
      var eyeClosed = '<svg viewBox="0 0 24 24"><path d="M12 7c2.76 0 5 2.24 5 5 0 .65-.13 1.26-.36 1.83l2.92 2.92c1.51-1.26 2.7-2.89 3.43-4.75-1.73-4.39-6-7.5-11-7.5-1.4 0-2.74.25-3.98.7l2.16 2.16C10.74 7.13 11.35 7 12 7zM2 4.27l2.28 2.28.46.46C3.08 8.3 1.78 10.02 1 12c1.73 4.39 6 7.5 11 7.5 1.55 0 3.03-.3 4.38-.84l.42.42L19.73 22 21 20.73 3.27 3 2 4.27zM7.53 9.8l1.55 1.55c-.05.21-.08.43-.08.65 0 1.66 1.34 3 3 3 .22 0 .44-.03.65-.08l1.55 1.55c-.67.33-1.41.53-2.2.53-2.76 0-5-2.24-5-5 0-.79.2-1.53.53-2.2zm4.31-.78l3.15 3.15.02-.16c0-1.66-1.34-3-3-3l-.17.01z"/></svg>';

      if (passwordField.type === "password") {
        passwordField.type = "text";
        toggleButton.innerHTML = eyeClosed;
        toggleButton.title = "Hide password";
      } else {
        passwordField.type = "password";
        toggleButton.innerHTML = eyeOpen;
        toggleButton.title = "Show password";
      }
    }

    function toggleStaticIpFields() {
      var staticIpFields = document.getElementById("staticIpFields");
      var useStatic = document.getElementById("useStatic");
      staticIpFields.style.display = useStatic.checked ? "block" : "none";
    }

    // Initialize fields visibility on page load
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

    // Percent mode means nothing without a trusted bottomPosition (only
    // established by a completed home), unlike a raw step count, which is
    // still meaningful enough for bench testing/jogging even when not
    // homed - the same reasoning handleMove()'s relative jog has always
    // used server-side. Disables both "Percent" radios (Status tab and
    // Settings tab) when not homed, and falls back to "Steps" mode if
    // Percent happened to be selected already, so a stale selection can't
    // silently misbehave. Called from the status poll whenever data.homed
    // changes.
    function updatePercentModeAvailability(homed) {
      [
        {radio: 'positionModePercentStatus', label: 'positionModePercentLabelStatus', group: 'positionModeStatus'},
        {radio: 'positionModePercent', label: 'positionModePercentLabel', group: 'positionMode'}
      ].forEach(function(entry) {
        var radio = document.getElementById(entry.radio);
        var label = document.getElementById(entry.label);
        if (!radio || !label) return;
        radio.disabled = !homed;
        label.classList.toggle('option-disabled', !homed);
        if (!homed && radio.checked) {
          var stepsRadio = document.querySelector('input[name="' + entry.group + '"][value="steps"]');
          if (stepsRadio) stepsRadio.checked = true;
        }
      });
    }

    function setPositionStatus() {
      var inputValue = document.getElementById("setPositionInputStatus").value;
      var mode = document.querySelector('input[name="positionModeStatus"]:checked').value;
      var position;

      if (mode === "percent") {
        var bottomPos = parseInt(document.getElementById('bottom-position').textContent);
        position = Math.round((bottomPos * inputValue) / 100);
        console.log('Moving to ' + inputValue + '% (position ' + position + ')');
      } else {
        position = inputValue;
        console.log('Moving to position ' + position);
      }

      fetch('/set-position?position=' + position)
        .then(response => response.text().then(text => {
          if (!response.ok) {
            showNotification(text, false);
          } else {
            console.log('Move command sent');
          }
        }));
    }

    function homeServo() {
      fetch('/home')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            showNotification(data.message, true);
          } else {
            showNotification('Homing failed: ' + data.message, false);
          }
        })
        .catch(error => {
          showNotification('Error starting homing: ' + error, false);
        });
    }

    var locateModeActive = false;
    function toggleLocate() {
      locateModeActive = !locateModeActive;
      var bannerSpider = document.getElementById('banner-spider');

      fetch('/locate?enable=' + locateModeActive)
        .then(response => response.json())
        .then(data => {
          if (data.locateMode) {
            bannerSpider.classList.add('active');
            console.log('Locate mode enabled - LED showing SOS');
          } else {
            bannerSpider.classList.remove('active');
            console.log('Locate mode disabled');
          }
        })
        .catch(error => {
          // If fetch fails (e.g., in preview mode), just toggle the class for visual testing
          console.log('Error toggling locate mode: ' + error);
          bannerSpider.classList.toggle('active');
        });
    }

    function rebootDevice() {
      if (!confirm('Reboot device?')) {
        return;
      }

      // Show notification that device is rebooting
      var notification = document.createElement('div');
      notification.className = 'notification success';
      notification.textContent = 'Device is rebooting... Page will refresh in a few seconds.';
      notification.style.display = 'block';
      document.body.appendChild(notification);

      // Send reboot command
      fetch('/reboot')
        .then(response => {
          // Wait 5 seconds then reload the page
          setTimeout(function() {
            window.location.reload();
          }, 5000);
        })
        .catch(error => {
          // Device already rebooting, wait and reload
          setTimeout(function() {
            window.location.reload();
          }, 5000);
        });
    }

    function resetSettings() {
      if (!confirm('Reset all settings to default values? This will reset WiFi credentials, stepper settings, DDP/LED configuration, etc. The device will reboot after reset.')) {
        return;
      }

      // Show notification that settings are being reset
      var notification = document.createElement('div');
      notification.className = 'notification success';
      notification.textContent = 'Resetting settings to defaults... Device will reboot.';
      notification.style.display = 'block';
      document.body.appendChild(notification);

      // Send reset command
      fetch('/reset-settings')
        .then(response => response.text())
        .then(data => {
          // Wait 5 seconds then reload the page
          setTimeout(function() {
            window.location.reload();
          }, 5000);
        })
        .catch(error => {
          // Device may already be rebooting, wait and reload
          setTimeout(function() {
            window.location.reload();
          }, 5000);
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
          // Decode wifiMode enum: 0=AP, 1=Client
          document.getElementById('wifi-mode').textContent = data.wifiMode === 1 ? 'Client Mode' : 'Access Point Mode';
          document.getElementById('wifi-network').textContent = data.wifiNetwork;
          // Decode ipType enum: 0=N/A, 1=DHCP, 2=Static
          var ipTypeStr = data.ipType === 2 ? 'Static IP' : (data.ipType === 1 ? 'DHCP' : 'N/A');
          document.getElementById('ip-type').textContent = ipTypeStr;
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
          } else if (data.homingError) {
            document.getElementById('homed-status-text').textContent = 'Homing Error';
            document.getElementById('homed-status-text').className = 'status homing-error';
          } else {
            document.getElementById('homed-status-text').textContent = data.homed ? 'Homed' : 'Not Homed';
            document.getElementById('homed-status-text').className = 'status ' + (data.homed ? 'homed' : 'not-homed');
          }
          updatePercentModeAvailability(data.homed);

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

          // Real physical speed limit, one-way (0-100%) - derived from the
          // last completed homing cycle (see homingTravelMs's declaration
          // comment, stepper_handler.h) - lets whoever is timing cues to
          // music know the device's actual achievable speed, not just
          // "seems fast enough on the bench".
          var homingTravelElement = document.getElementById('homing-travel');
          if (data.homingTravelValid && data.homingTravelMs > 0) {
            var travelSecs = (data.homingTravelMs / 1000).toFixed(1);
            var stepsPerSec = Math.round(data.homingTravelSteps * 1000 / data.homingTravelMs);
            homingTravelElement.textContent = data.homingTravelSteps + ' steps in ' + travelSecs + 's (~' + stepsPerSec + ' steps/s)';
          } else {
            homingTravelElement.textContent = 'Not yet measured';
          }

          // Rotary encoder on the motor shaft - ground-truth position cross-
          // check, independent of the stepper's own step-count bookkeeping.
          // Resyncs to 0 at the end of every successful homing (see
          // resetEncoderCount()'s call site, stepper_handler.cpp).
          // encoderMissed is a lifetime count of "illegal 2-bit jump" ISR
          // events - a real quadrature edge that was never sampled and is
          // permanently lost (see getMissedTransitionCount()'s declaration
          // comment, encoder_handler.h). Near 0 across a real test means
          // firmware isn't the source of any remaining drift/noise.
          document.getElementById('encoder-count').textContent = data.encoderInitialized ?
            (data.encoderCount + ' (' + data.encoderMissed + ' missed)') : 'N/A';

          // Update Stepper position command
          document.getElementById('position-command').textContent = data.protocolLastCommand;
          document.getElementById('position-command-percent').textContent = data.protocolLastCommandPercent;

          // Update DDP status
          document.getElementById('protocol-mode').textContent = data.control16Bit ? '16-bit' : '8-bit';
          document.getElementById('total-channels').textContent = data.totalChannels;
          document.getElementById('protocol-packets-received').textContent = data.protocolPacketsReceived;

          // Update LED status
          var ledMaxPixelsReceived = data.ledMaxPixelsReceived || 0;
          var ledPixelCount = data.ledPixelCount || 0;
          var ledsBlanked = data.ledsBlanked || false;

          document.getElementById('led-pixel-count').textContent = ledPixelCount;

          // Received pixel count, shown alongside packets received
          var receivedElement = document.getElementById('led-pixels-received');
          receivedElement.textContent = ledMaxPixelsReceived;
          if (ledsBlanked) {
            receivedElement.style.color = '#6c757d';  // Gray - no recent data
          } else {
            receivedElement.style.color = (ledMaxPixelsReceived > ledPixelCount && ledPixelCount > 0) ? '#dc3545' : 'inherit';
          }

          // DDP state indicator: 0=Disabled (OTA in progress), 1=Enabled, 2=Paused (test mode)
          var ddpStatusEl = document.getElementById('ddp-status-text');
          if (ddpStatusEl) {
            if (data.ddpState === 0) {
              ddpStatusEl.textContent = 'Disabled';
              ddpStatusEl.className = 'status disconnected';
            } else if (data.ddpState === 2) {
              ddpStatusEl.textContent = 'Paused';
              ddpStatusEl.className = 'status not-homed';
            } else {
              ddpStatusEl.textContent = 'Enabled';
              ddpStatusEl.className = 'status connected';
            }
          }

          // Time since last DDP packet
          var lastPacketEl = document.getElementById('ddp-last-packet');
          if (lastPacketEl) {
            lastPacketEl.textContent = data.ddpEverReceived ? (data.secsSinceLastDdp + 's ago') : 'Never';
          }

          // Fetch LED preview separately (only if enabled to save bandwidth)
          if (ledPreviewEnabled) {
            fetchLedPreview();
          }

          // Sync LED test mode checkbox across all clients
          var ledTestCheckbox = document.getElementById('led-test-mode-enabled');
          if (ledTestCheckbox && document.activeElement !== ledTestCheckbox) {
            ledTestCheckbox.checked = data.ledTestMode || false;
          }

          // Update locate mode indicator to sync across all clients
          var bannerSpider = document.getElementById('banner-spider');
          if (data.locateMode) {
            if (bannerSpider && !bannerSpider.classList.contains('active')) {
              bannerSpider.classList.add('active');
              locateModeActive = true;
            }
          } else {
            if (bannerSpider && bannerSpider.classList.contains('active')) {
              bannerSpider.classList.remove('active');
              locateModeActive = false;
            }
          }

          // Update TMC2209 driver status
          var tmcBox = document.getElementById('tmc-status-box');
          if (data.tmcEnabled) {
            tmcBox.style.display = 'block';

            var tmcConnEl = document.getElementById('tmc-connected-status');
            tmcConnEl.textContent = data.tmcConnected ? 'Connected' : 'Not Connected';
            tmcConnEl.className = 'status ' + (data.tmcConnected ? 'homed' : 'not-homed');

            var diagEl = document.getElementById('tmc-diag-status');
            var diagIssues = [];
            if (data.tmcOverTempShutdown) diagIssues.push('OVER-TEMP SHUTDOWN');
            if (data.tmcOverTempWarning) diagIssues.push('over-temp warning');
            if (data.tmcShortToGroundA || data.tmcShortToGroundB) diagIssues.push('short to ground');
            if (data.tmcOpenLoadA || data.tmcOpenLoadB) diagIssues.push('open load');
            if (data.tmcUartCrcError) diagIssues.push('UART CRC errors');
            diagEl.textContent = diagIssues.length > 0 ? diagIssues.join(', ') : 'OK';
            diagEl.style.color = diagIssues.length > 0 ? '#dc3545' : 'inherit';

            var stallEl = document.getElementById('tmc-stall-status');
            var clearStallBtn = document.getElementById('tmc-clear-stall-btn');
            if (!data.tmcStallEnabled) {
              stallEl.textContent = 'Disabled';
              stallEl.style.color = 'inherit';
              clearStallBtn.style.display = 'none';
            } else if (data.tmcStalled) {
              stallEl.textContent = 'STALLED (SG_RESULT ' + data.tmcStallGuardResult + ')';
              stallEl.style.color = '#dc3545';
              clearStallBtn.style.display = 'inline-block';
            } else {
              stallEl.textContent = 'OK (live SG_RESULT ' + data.tmcStallGuardResult + ')';
              stallEl.style.color = 'inherit';
              clearStallBtn.style.display = 'none';
            }
          } else {
            tmcBox.style.display = 'none';
          }

          // Update auto home on boot status (text on status page only, not checkbox on settings page)
          var autoHomeStatus = document.getElementById('auto-home-on-boot');
          if (autoHomeStatus) {
            autoHomeStatus.textContent = data.autoHomeOnBoot ? 'Enabled' : 'Disabled';
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

// OTA Update JavaScript - wrap in DOMContentLoaded to ensure form exists
    document.addEventListener('DOMContentLoaded', function() {
      var otaForm = document.getElementById('otaForm');
      if (!otaForm) {
        return;
      }

      otaForm.addEventListener('submit', function(e) {
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

      // Reset and show progress
      var progressBar = document.getElementById('progressBar');
      var progressText = document.getElementById('progressText');
      var updateProgress = document.getElementById('updateProgress');
      var updateStatus = document.getElementById('updateStatus');
      var successBanner = document.getElementById('successBanner');

      // Reset progress
      progressBar.style.width = '0%';
      progressBar.style.backgroundColor = '#007bff';
      progressText.textContent = '0%';
      updateStatus.textContent = 'Uploading firmware...';
      updateStatus.style.color = '#333';
      successBanner.style.display = 'none';

      // Show progress section
      updateProgress.style.display = 'block';
      document.getElementById('updateButton').disabled = true;

      var formData = new FormData();
      formData.append('firmware', file);

      var xhr = new XMLHttpRequest();

      // Progress handler
      xhr.upload.addEventListener('progress', function(e) {
        if (e.lengthComputable) {
          var percentComplete = Math.round((e.loaded / e.total) * 100);
          progressBar.style.width = percentComplete + '%';
          progressText.textContent = percentComplete + '%';
          updateStatus.textContent = 'Uploading: ' + percentComplete + '%';
        }
      });

      // Completion handler
      xhr.addEventListener('load', function() {
        if (xhr.status === 200) {
          document.getElementById('progressBar').style.width = '100%';
          document.getElementById('progressText').textContent = '100%';
          document.getElementById('progressBar').style.backgroundColor = '#28a745';

          // Hide progress bar and show success banner
          document.getElementById('updateProgress').style.display = 'none';
          var successBanner = document.getElementById('successBanner');
          successBanner.style.display = 'block';

          // Start countdown
          var countdown = 10;
          var countdownEl = document.getElementById('countdown');
          countdownEl.textContent = countdown;

          var countdownInterval = setInterval(function() {
            countdown--;
            countdownEl.textContent = countdown;
            if (countdown <= 0) {
              clearInterval(countdownInterval);
              window.location.reload();
            }
          }, 1000);
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
    }); // End DOMContentLoaded
  </script>
</head>
<body>
  <!-- Full-Width Banner Header -->
  <div class="banner-header">
    <div class="banner-content">
      <!-- Spider Icon (clickable for locate) -->
      <div id="banner-spider" class="banner-spider" onclick="toggleLocate()" title="Click to locate this device (LED will blink SOS)">
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="60" height="60" class="spider-svg">
          <g transform="translate(50, 50)">
            <!-- Body -->
            <circle cx="0" cy="0" r="14" class="spider-body" fill="#00d4ff"/>
            <circle cx="0" cy="-9" r="9" class="spider-head" fill="#00d4ff"/>
            <!-- Legs (4 pairs) -->
            <!-- Left legs -->
            <path d="M -12,-8 L -25,-18 L -32,-15" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M -12,-2 L -28,-5 L -35,-2" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M -12,4 L -28,8 L -35,12" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M -12,10 L -25,18 L -32,22" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <!-- Right legs -->
            <path d="M 12,-8 L 25,-18 L 32,-15" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M 12,-2 L 28,-5 L 35,-2" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M 12,4 L 28,8 L 35,12" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <path d="M 12,10 L 25,18 L 32,22" class="spider-leg" stroke="#00d4ff" stroke-width="2" fill="none" stroke-linecap="round"/>
            <!-- Eyes -->
            <circle cx="-4" cy="-12" r="2" class="spider-eyes" fill="#ff0000"/>
            <circle cx="4" cy="-12" r="2" class="spider-eyes" fill="#ff0000"/>
          </g>
        </svg>
      </div>

      <div class="banner-title">
        Servo Controller
        <div class="banner-hostname" id="hostname-display">{{HOSTNAME}}</div>
      </div>
    </div>
  </div>

  <!-- Blocky Tab Headers -->
  <div class="tabs">
    <button class="tab active" onclick="showTab('status-tab')">Status</button>
    <button class="tab" onclick="showTab('settings-tab')">Settings</button>
    <button class="tab" onclick="showTab('update-tab')">OTA Update</button>
  </div>

  <div class="container">
    <div class="content-wrapper">

    <!-- Status Tab -->
    <div id="status-tab" class="tab-content active">
      <div class="status-grid">
      <div class="status-box">
        <h4>WiFi Status</h4>
        <div id="wifi-status-text" class="status {{STATUS_CLASS}}">
          {{STATUS_TEXT}}
        </div>
        <p><strong>Hostname:</strong> <span id="wifi-hostname">{{HOSTNAME}}</span></p>
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
        <p><strong>Switch:</strong> <span id="homing-switch-status" class="status">Not Tripped</span></p>
        <p><strong>Position Command:</strong> <span id="position-command">0</span> (<span id="position-command-percent">0.0</span>%)</p>
        <p><strong>Current Position:</strong> <span id="current-position">{{CURRENT_POSITION}}</span> steps (<span id="position-percent">{{POSITION_PERCENT}}</span>%)</p>
        <p><strong>Bottom Position:</strong> <span id="bottom-position">{{BOTTOM_POSITION}}</span> steps</p>
        <p><strong>Full Travel:</strong> <span id="homing-travel">{{HOMING_TRAVEL}}</span></p>
        <p><strong>Encoder Count:</strong> <span id="encoder-count">{{ENCODER_COUNT}}</span></p>
        <p><strong>Auto Home on Boot:</strong> <span id="auto-home-on-boot">{{AUTO_HOME_STATUS}}</span></p>

        <button class="collapsible" onclick="toggleCollapsible(this)">Manual Stepper Control</button>
        <div class="collapsible-content">
          <div class="collapsible-content-inner">
            <h4 style="margin-top: 0;">Position Control</h4>

            <div class="form-group">
              <label for="setPositionInputStatus">Move to Position:</label>
              <div class="position-input-group">
                <input type="number" id="setPositionInputStatus" name="setPositionInputStatus" value="0" min="0" max="200">
                <div class="position-controls">
                  <div class="position-mode-selector">
                    <label>
                      <input type="radio" name="positionModeStatus" value="steps" checked>
                      Steps
                    </label>
                    <label id="positionModePercentLabelStatus" title="0-100% is normal travel range. 100-200% deliberately overshoots past the bottom to the far switch-trigger point (the rope wrapped the other way) - for setting up a starting position to test homing from, not normal operation.">
                      <input type="radio" name="positionModeStatus" value="percent" id="positionModePercentStatus">
                      Percent (0-200%)
                    </label>
                  </div>
                  <button onclick="setPositionStatus()" class="btn-warning">Go</button>
                </div>
              </div>
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

        <div id="tmc-status-box" style="display: none; margin-top: 10px; padding-top: 10px; border-top: 1px solid #ddd;">
          <h4>Driver Status (TMC2209)</h4>
          <p><strong>UART Link:</strong> <span id="tmc-connected-status" class="status">Not Connected</span></p>
          <p><strong>Diagnostics:</strong> <span id="tmc-diag-status">OK</span></p>
          <p><strong>Stall Guard:</strong> <span id="tmc-stall-status">Disabled</span></p>
          <button id="tmc-clear-stall-btn" class="btn-warning" style="display: none;" onclick="clearTmcStall()">Clear Stall Fault</button>
        </div>
      </div>

      <div class="status-box">
        <h4>DDP / LED Status</h4>
        <div id="ddp-status-text" class="status">Enabled</div>
        <p><strong>Stepper Mode:</strong> <span id="protocol-mode">8-bit</span></p>
        <p><strong>Total Channels:</strong> <span id="total-channels">0</span></p>
        <p><strong>Packets Received:</strong> <span id="protocol-packets-received">0</span> (<span id="led-pixels-received">0</span> pixels)</p>
        <p><strong>Last Packet:</strong> <span id="ddp-last-packet">Never</span></p>

        <div style="margin-top: 10px;">
          <label style="font-size: 12px; cursor: pointer;">
            <input type="checkbox" id="led-preview-enabled" onchange="toggleLedPreview()">
            Show Pixels
          </label>
        </div>
        <div id="led-preview-container" class="led-preview-box" style="margin-bottom: 10px; max-height: 300px; overflow-y: auto; overflow-x: hidden; display: none; padding: 5px; border-radius: 4px;">
          <div id="led-preview" style="display: flex; flex-wrap: wrap; gap: 1px;"></div>
        </div>
        <p><strong>Configured Pixels:</strong> <span id="led-pixel-count">0</span></p>

        <div style="margin-top: 10px;">
          <label style="font-size: 12px; cursor: pointer;">
            <input type="checkbox" id="led-test-mode-enabled" onchange="toggleLedTestMode()">
            Test Pattern
          </label>
        </div>
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
            <label for="hostname">Hostname:</label>
            <input type="text" id="hostname" name="hostname" value="{{HOSTNAME}}" required pattern="[a-zA-Z0-9\-]+" title="Only letters, numbers, and hyphens allowed">
          </div>

          <div class="form-group">
            <label for="ssid">WiFi Network (SSID):</label>
            <input type="text" id="ssid" name="ssid" value="{{CURRENT_SSID}}" required>
          </div>

          <div class="form-group">
            <label for="password">Password:</label>
            <div class="password-wrapper">
              <input type="password" id="password" name="password" placeholder="Enter WiFi password">
              <button type="button" class="password-toggle" id="passwordToggle" onclick="togglePassword('password', 'passwordToggle')" title="Show password">
                <svg viewBox="0 0 24 24"><path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg>
              </button>
            </div>
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
            <div class="password-wrapper">
              <input type="password" id="apPassword" name="apPassword" value="{{AP_PASSWORD}}" required minlength="8">
              <button type="button" class="password-toggle" id="apPasswordToggle" onclick="togglePassword('apPassword', 'apPasswordToggle')" title="Show password">
                <svg viewBox="0 0 24 24"><path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/></svg>
              </button>
            </div>
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
            <label for="stepperSpeedHoming">Homing Speed (Hz):</label>
            <input type="number" id="stepperSpeedHoming" name="stepperSpeedHoming" value="{{STEPPER_SPEED_HOMING}}" min="100" max="50000" required>
          </div>

          <div class="form-group">
            <label for="stepperAccelHoming">Homing Acceleration (steps/s&sup2;):</label>
            <input type="number" id="stepperAccelHoming" name="stepperAccelHoming" value="{{STEPPER_ACCEL_HOMING}}" min="0" max="2000000" required>
          </div>

          <div class="form-group">
            <label for="stepperTrackMode">Tracking Motion Strategy:</label>
            <select id="stepperTrackMode" name="stepperTrackMode" style="width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; border: 2px solid #ddd; border-radius: 4px;">
              <option value="0" {{TRACK_MODE_DIRECT_SEL}}>Direct (moveTo every packet)</option>
              <option value="4" {{TRACK_MODE_PID_SEL}}>PID (closed-loop tracking with velocity feedforward)</option>
            </select>
          </div>

          <div class="form-group">
            <label for="stepperTrackEnabled">
              <input type="checkbox" id="stepperTrackEnabled" name="stepperTrackEnabled" {{STEPPER_TRACK_ENABLED_CHECKED}}>
              Small-Move Tracking
            </label>
            <p style="color: #666; font-size: 12px; margin: 4px 0 0;">Direct mode only - PID has its own closed-loop control instead. Uses a gentler speed/acceleration for small position updates (e.g. xLights slowly panning a value over DDP), so a stream of tiny moves blends into smooth continuous motion instead of a torque-spiking accelerate/decelerate cycle on every packet.</p>
          </div>

          <button type="button" class="collapsible" onclick="toggleCollapsible(this)">Small-Move Tracking Settings</button>
          <div class="collapsible-content">
            <div class="collapsible-content-inner">
              <div class="form-group">
                <label for="stepperTrackThreshold">Tracking Threshold (steps):</label>
                <input type="number" id="stepperTrackThreshold" name="stepperTrackThreshold" value="{{STEPPER_TRACK_THRESHOLD}}" min="1" max="100000" required>
                <p style="color: #666; font-size: 12px; margin: 4px 0 0;">Moves at or below this size (measured from the previous commanded position, not the motor's actual position) use the tracking profile; anything bigger uses the normal profile.</p>
              </div>

              <div class="form-group">
                <label for="stepperTrackMaxLag">Tracking Max Lag (steps):</label>
                <input type="number" id="stepperTrackMaxLag" name="stepperTrackMaxLag" value="{{STEPPER_TRACK_MAX_LAG}}" min="1" max="1000000" required>
                <p style="color: #666; font-size: 12px; margin: 4px 0 0;">Safety net: if the motor's actual position ever falls this far behind the commanded target, the normal profile is used regardless of the setting above, to resync rather than let it drift indefinitely.</p>
              </div>

              <div class="form-group">
                <label for="stepperTrackSpeed">Tracking Speed (Hz):</label>
                <input type="number" id="stepperTrackSpeed" name="stepperTrackSpeed" value="{{STEPPER_TRACK_SPEED}}" min="10" max="50000" required>
              </div>

              <div class="form-group">
                <label for="stepperTrackAccel">Tracking Acceleration (steps/s&sup2;):</label>
                <input type="number" id="stepperTrackAccel" name="stepperTrackAccel" value="{{STEPPER_TRACK_ACCEL}}" min="1" max="1000000" required>
              </div>
            </div>
          </div>

          <div class="form-group">
            <label for="autoHomeOnBoot">
              <input type="checkbox" id="autoHomeOnBoot" name="autoHomeOnBoot" {{AUTO_HOME_ON_BOOT_CHECKED}}>
              Auto Home on Bootup
            </label>
          </div>

          <button type="submit" class="btn-primary">Save Stepper Settings</button>
        </form>

        <button type="button" class="collapsible" onclick="toggleCollapsible(this)">TMC2209 Config</button>
        <div class="collapsible-content">
          <div class="collapsible-content-inner">
            <p style="color: #666; font-style: italic; margin-top: 0;">Digital current control, StallGuard-based jam detection, and driver diagnostics over the UART link on D6/D7. Always enabled - if the driver isn't wired for UART, the link simply won't connect (see "UART Link" on the Status tab).</p>

            <form onsubmit="return handleFormSubmit(event, '/save-tmc')">
              <div class="form-group">
                <label for="tmcRunCurrent">Run Current (mA):</label>
                <input type="number" id="tmcRunCurrent" name="tmcRunCurrent" value="{{TMC_RUN_CURRENT}}" min="0" max="2000" required>
              </div>

              <div class="form-group">
                <label for="tmcHoldPercent">Hold Current (% of run):</label>
                <input type="number" id="tmcHoldPercent" name="tmcHoldPercent" value="{{TMC_HOLD_PERCENT}}" min="0" max="100" required>
              </div>

              <div class="form-group">
                <label for="tmcStallEnabled">
                  <input type="checkbox" id="tmcStallEnabled" name="tmcStallEnabled" {{TMC_STALL_ENABLED_CHECKED}}>
                  Enable Stall Detection Safety Cutoff
                </label>
              </div>

              <div class="form-group">
                <label for="tmcStallThreshold">Stall Threshold (live SG_RESULT below this = stalled; tune on the bench, watch the live value on the Status tab):</label>
                <input type="number" id="tmcStallThreshold" name="tmcStallThreshold" value="{{TMC_STALL_THRESHOLD}}" min="0" max="1023" required>
              </div>

              <div class="form-group">
                <label for="tmcMicrosteps">Microsteps per Full Step:</label>
                <select id="tmcMicrosteps" name="tmcMicrosteps" style="width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; border: 2px solid #ddd; border-radius: 4px;">
                  <option value="1" {{TMC_USTEP_1}}>1 (full step)</option>
                  <option value="2" {{TMC_USTEP_2}}>2</option>
                  <option value="4" {{TMC_USTEP_4}}>4</option>
                  <option value="8" {{TMC_USTEP_8}}>8</option>
                  <option value="16" {{TMC_USTEP_16}}>16</option>
                  <option value="32" {{TMC_USTEP_32}}>32</option>
                  <option value="64" {{TMC_USTEP_64}}>64</option>
                  <option value="128" {{TMC_USTEP_128}}>128</option>
                  <option value="256" {{TMC_USTEP_256}}>256 (finest, slowest)</option>
                </select>
                <p style="color: #666; font-size: 12px; margin: 4px 0 0;">Changing this changes physical distance per step - re-home afterward and expect Stepper Speed (Hz) to feel different.</p>
              </div>

              <button type="submit" class="btn-primary">Save Driver Settings</button>
            </form>
          </div>
        </div>
      </div>

      <!-- Channel + LED Configuration Box -->
      <div class="status-box">
        <h4>Channel &amp; LED Configuration</h4>
        <p style="color: #666; font-style: italic;">Position and pixel data are received via DDP on port 4048. No protocol selection needed.</p>

        <form onsubmit="return handleFormSubmit(event, '/save-protocol')">
          <div class="form-group">
            <label for="stepperControl">
              <input type="checkbox" id="stepperControl" name="stepperControl" {{STEPPER_CONTROL_CHECKED}} onchange="toggleStepperOptions()">
              Enable Stepper Control (uses channel 1)
            </label>
          </div>

          <div class="form-group" id="control16BitGroup">
            <label for="control16Bit">
              <input type="checkbox" id="control16Bit" name="control16Bit" {{CONTROL_16BIT_CHECKED}}>
              16-bit Stepper Control (uses channels 1-2)
            </label>
          </div>

          <div class="form-group" id="stepperBlankTimeGroup">
            <label for="stepperBlankTime">Stepper Blank Time (seconds, 0=disabled):</label>
            <input type="number" id="stepperBlankTime" name="stepperBlankTime" value="{{STEPPER_BLANK_TIME}}" min="0" max="3600" required>
          </div>

          <div class="form-group">
            <label for="protocolDebug">
              <input type="checkbox" id="protocolDebug" name="protocolDebug" {{PROTOCOL_DEBUG_CHECKED}}>
              Enable Serial Debug Output
            </label>
          </div>

          <div class="form-group">
            <label for="ledBlankTime">LED Blank Time (seconds, 0=disabled):</label>
            <input type="number" id="ledBlankTime" name="ledBlankTime" value="{{LED_BLANK_TIME}}" min="0" max="3600" required>
          </div>

          <button type="submit" class="btn-primary">Save Channel Settings</button>
        </form>

        <hr style="margin: 30px 0; border: none; border-top: 1px solid #ddd;">

        <h3>LED Configuration</h3>

        <form onsubmit="return handleFormSubmit(event, '/save-led')">
          <div class="form-group">
            <label for="ledPixelCount">Pixel Count:</label>
            <input type="number" id="ledPixelCount" name="ledPixelCount" value="{{LED_PIXEL_COUNT}}" min="0" max="1000" required>
            <p style="color: #666; font-size: 12px; margin: 4px 0 0;">Large pixel counts add real WiFi/DDP bandwidth and per-frame update time - a few hundred pixels is a safer practical ceiling than the 1000 the field allows; test on the bench before committing to a big run.</p>
          </div>

          <div class="form-group">
            <label for="ledColorOrder">Color Order:</label>
            <select id="ledColorOrder" name="ledColorOrder" style="width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; border: 2px solid #ddd; border-radius: 4px;">
              <option value="RGB" {{LED_ORDER_RGB}}>RGB</option>
              <option value="RBG" {{LED_ORDER_RBG}}>RBG</option>
              <option value="GRB" {{LED_ORDER_GRB}}>GRB</option>
              <option value="GBR" {{LED_ORDER_GBR}}>GBR</option>
              <option value="BRG" {{LED_ORDER_BRG}}>BRG</option>
              <option value="BGR" {{LED_ORDER_BGR}}>BGR</option>
            </select>
          </div>

          <div class="form-group">
            <label for="ledGamma">Gamma Value:</label>
            <input type="number" id="ledGamma" name="ledGamma" value="{{LED_GAMMA}}" min="0.1" max="5.0" step="0.1" required>
          </div>

          <div class="form-group">
            <label for="ledBrightness">Brightness (%):</label>
            <input type="number" id="ledBrightness" name="ledBrightness" value="{{LED_BRIGHTNESS}}" min="0" max="100" required>
          </div>

          <div class="form-group">
            <label for="ledStartNullPixels">Start Null Pixels:</label>
            <input type="number" id="ledStartNullPixels" name="ledStartNullPixels" value="{{LED_START_NULL}}" min="0" max="100" required>
          </div>

          <div class="form-group">
            <label for="ledEndNullPixels">End Null Pixels:</label>
            <input type="number" id="ledEndNullPixels" name="ledEndNullPixels" value="{{LED_END_NULL}}" min="0" max="100" required>
          </div>

          <button type="submit" class="btn-primary">Save LED Settings</button>
        </form>
      </div>

      <!-- System Control Box -->
      <div class="status-box">
        <h4>System Control</h4>
        <button onclick="rebootDevice()" style="width: 100%; margin-bottom: 10px;" class="btn-danger">Reboot Device</button>
        <button onclick="resetSettings()" style="width: 100%;" class="btn-danger">Reset Settings</button>
      </div>
      </div>
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

        <!-- Success Banner -->
        <div id="successBanner" style="display: none; margin-top: 20px; padding: 20px; background: linear-gradient(135deg, #28a745 0%, #20c997 100%); border-radius: 8px; text-align: center; color: white; box-shadow: 0 4px 6px rgba(0,0,0,0.1);">
          <h3 style="margin: 0 0 10px 0; font-size: 24px;">&#x2713; Update Successful!</h3>
          <p style="margin: 0 0 5px 0; font-size: 16px;">Device is rebooting with new firmware</p>
          <p style="margin: 0; font-size: 14px; opacity: 0.9;">Page will refresh in <span id="countdown" style="font-weight: bold; font-size: 18px;">10</span> seconds...</p>
        </div>
      </div>
    </div>

    <p style="text-align: center; color: #6a7a8a; font-size: 12px; margin-top: 20px; padding-bottom: 10px;">
      Version {{VERSION}} | Built: {{BUILD_DATE}} {{BUILD_TIME}}
    </p>
    </div>
  </div>
</body>
</html>
)rawliteral";
