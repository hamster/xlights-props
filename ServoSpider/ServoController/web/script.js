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

    function toggleTmcOptions() {
      var tmcEnabled = document.getElementById('tmcEnabled').checked;
      document.getElementById('tmcOptionsGroup').style.display = tmcEnabled ? 'block' : 'none';
    }

    function toggleTrackModeFields() {
      var mode = document.getElementById('stepperTrackMode').value;
      document.getElementById('coalesceFieldsGroup').style.display = (mode === '1') ? 'block' : 'none';
      document.getElementById('streamFieldsGroup').style.display = (mode === '2') ? 'block' : 'none';
      document.getElementById('lookaheadFieldsGroup').style.display = (mode === '3') ? 'block' : 'none';
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

    function toggleCompactLog() {
      var enabled = document.getElementById('compact-log-enabled').checked;
      fetch('/compact-log?enable=' + enabled)
        .then(response => response.json())
        .then(data => {
          console.log('Compact motion log: ' + (data.compactLog ? 'enabled' : 'disabled'));
        })
        .catch(error => {
          showNotification('Error toggling compact log: ' + error, false);
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
      toggleTmcOptions();
      toggleTrackModeFields();
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
      var inputValue = document.getElementById("setPositionInput").value;
      var mode = document.querySelector('input[name="positionMode"]:checked').value;
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

          // Sync compact motion log checkbox across all clients
          var compactLogCheckbox = document.getElementById('compact-log-enabled');
          if (compactLogCheckbox && document.activeElement !== compactLogCheckbox) {
            compactLogCheckbox.checked = data.compactLog || false;
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