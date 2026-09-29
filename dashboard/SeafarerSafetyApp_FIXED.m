classdef SeafarerSafetyApp_FIXED < matlab.apps.AppBase
% ============================================================================
%  Seafarer Safety Monitor - MATLAB dashboard (CORRECTED)
% ----------------------------------------------------------------------------
%  Fixes vs. SeafarerSafetyApp.m:
%   1. RISK MAPPING WAS INVERTED. The old file did:
%          if riskScore == 1 -> 'Safe'; elseif == 2 -> 'Moderate'; else 'High'
%      so riskScore == 0 (nothing wrong) fell into ELSE and displayed
%      "High Risk". Now 0 -> Safe, 1 -> Moderate, 2+ -> High.
%   2. "any(waterLevelData > 1)" could NEVER be true while the water sensor was
%      a digitalRead (0 or 1). Verified across all 940 pre-August entries: max
%      value is 1. Since the Aug-2026 rewiring the probe is read as an analog
%      value, so the rule now uses a calibrated threshold (WaterThreshold).
%   3. "mean(memsData) > 2000" could NEVER be true while the firmware divided
%      the ADC by 4 (max 1023). Verified: 0 of 919 readings ever exceeded 2000.
%      Now uses peak-to-peak movement on the raw 0..4095 signal, which is what
%      actually indicates a fall / impact / heavy motion.
%   4. Three separate thingSpeakRead calls every 5 s -> one call. The old code
%      made 36 API calls a minute to re-read data that only updates every 30 s.
%   5. Added NaN / empty-channel guards, a staleness warning, and an
%      all-zeros warning so a wiring fault is visible on the dashboard.
%   6. Start button no longer leaks a timer if pressed twice.
% ============================================================================

    properties (Access = public)
        UIFigure         matlab.ui.Figure
        StartButton      matlab.ui.control.Button
        StopButton       matlab.ui.control.Button
        RiskStatusLabel  matlab.ui.control.Label
        DetailLabel      matlab.ui.control.Label
        MEMSAxes         matlab.ui.control.UIAxes
        ECGAxes          matlab.ui.control.UIAxes
        WaterLevelAxes   matlab.ui.control.UIAxes
    end

    properties (Access = private)
        TimerObj   = []
        ChannelID  = 0     % your ThingSpeak channel ID
        ReadAPIKey = 'YOUR_THINGSPEAK_READ_KEY'
        NumPoints  = 20

        % ---- thresholds (tune these to your hardware) -------------------
        MemsPkPkThreshold = 400    % raw ADC counts peak-to-peak = motion
        BpmHigh           = 120    % tachycardia
        BpmLow            = 45     % bradycardia
        StaleMinutes      = 5      % newer than this = hardware is live
        WaterThreshold    = 350    % analog probe: dry 112-140, wet 564-687 counts
                                   % (re-measure after flashing firmware v2)
    end

    methods (Access = private)

        function updateData(app, ~, ~)

            % ---- ONE read for all three fields --------------------------
            try
                [data, timestamps] = thingSpeakRead(app.ChannelID, ...
                    'Fields',    [1 2 3], ...
                    'NumPoints', app.NumPoints, ...
                    'ReadKey',   app.ReadAPIKey);
            catch ME
                app.setStatus('No data', [0.4 0.4 0.4]);
                app.DetailLabel.Text = ['ThingSpeak read failed: ' ME.message];
                return;
            end

            if isempty(data)
                app.setStatus('No data', [0.4 0.4 0.4]);
                app.DetailLabel.Text = 'Channel returned no points.';
                return;
            end

            memsData  = data(:,1);
            ecgData   = data(:,2);
            waterData = data(:,3);

            % ---- is the hardware actually still alive? ------------------
            warnings = strings(0);
            tEnd = timestamps(end);
            if isempty(tEnd.TimeZone)
                tEnd.TimeZone = 'UTC';
            end
            ageMin = minutes(datetime('now','TimeZone','UTC') - tEnd);
            if ageMin > app.StaleMinutes
                warnings(end+1) = sprintf('newest point is %.0f min old - hardware offline?', ageMin);
            end

            % ---- is every sensor flatlined at zero? ---------------------
            if all(memsData == 0 | isnan(memsData)) && ...
               all(ecgData  == 0 | isnan(ecgData))  && ...
               all(waterData == 0 | isnan(waterData))
                warnings(end+1) = 'ALL sensors read 0 - wiring / common-GND fault, not a ThingSpeak fault';
            end

            % ---- risk calculation ---------------------------------------
            memsValid = memsData(~isnan(memsData));
            if numel(memsValid) >= 2
                memsPkPk = max(memsValid) - min(memsValid);
                memsRisk = memsPkPk > app.MemsPkPkThreshold;
            else
                memsPkPk = 0;
                memsRisk = false;
            end

            % field2 now carries BPM; 0 means "leads off / no valid reading"
            bpmValid = ecgData(ecgData > 0 & ~isnan(ecgData));
            if isempty(bpmValid)
                ecgRisk = false;
                bpmTxt  = 'no ECG';
            else
                meanBpm = mean(bpmValid);
                ecgRisk = (meanBpm > app.BpmHigh) || (meanBpm < app.BpmLow);
                bpmTxt  = sprintf('%.0f BPM', meanBpm);
            end

            waterRisk = any(waterData >= app.WaterThreshold);   % analog probe

            riskScore = double(memsRisk) + double(ecgRisk) + double(waterRisk);

            % ---- status --------------------------------------------------
            switch riskScore
                case 0
                    app.setStatus('Safe',          [0   0.5 0  ]);
                case 1
                    app.setStatus('Moderate Risk', [0.9 0.6 0  ]);
                otherwise
                    app.setStatus('High Risk',     [1   0   0  ]);
            end

            detail = sprintf('MEMS pk-pk %.0f (%d) | %s (%d) | Water %d (%d) | score %d', ...
                memsPkPk, memsRisk, bpmTxt, ecgRisk, max(waterData), waterRisk, riskScore);
            if ~isempty(warnings)
                detail = [detail '   ||  WARNING: ' char(strjoin(warnings, '; '))];
            end
            app.DetailLabel.Text = detail;
            disp(detail);

            % ---- plots ---------------------------------------------------
            plot(app.MEMSAxes, timestamps, memsData, '-b', 'LineWidth', 1.5);
            title(app.MEMSAxes, 'MEMS Sensor Data');
            ylabel(app.MEMSAxes, 'Raw ADC (0-4095)');
            grid(app.MEMSAxes, 'on');

            plot(app.ECGAxes, timestamps, ecgData, '-r', 'LineWidth', 1.5);
            title(app.ECGAxes, 'Heart Rate');
            ylabel(app.ECGAxes, 'BPM');
            grid(app.ECGAxes, 'on');

            plot(app.WaterLevelAxes, timestamps, waterData, '-k', 'LineWidth', 1.5);
            yline(app.WaterLevelAxes, app.WaterThreshold, '--r');
            title(app.WaterLevelAxes, 'Water Level Sensor');
            ylabel(app.WaterLevelAxes, 'Raw counts');
            grid(app.WaterLevelAxes, 'on');
        end

        function setStatus(app, txt, colour)
            app.RiskStatusLabel.Text      = txt;
            app.RiskStatusLabel.FontColor = colour;
        end

        function StartButtonPushed(app, ~)
            app.stopTimer();                      % never leak a timer
            app.TimerObj = timer( ...
                'ExecutionMode', 'fixedSpacing', ...
                'Period',        15, ...          % channel only updates every 30 s
                'TimerFcn',      @(~,~) app.updateData);
            start(app.TimerObj);
            app.updateData();                     % refresh immediately
        end

        function StopButtonPushed(app, ~)
            app.stopTimer();
        end

        function stopTimer(app)
            if ~isempty(app.TimerObj) && isvalid(app.TimerObj)
                stop(app.TimerObj);
                delete(app.TimerObj);
            end
            app.TimerObj = [];
        end
    end

    methods (Access = public)

        function app = SeafarerSafetyApp_FIXED

            app.UIFigure = uifigure( ...
                'Position', [100 100 640 640], ...
                'Name',     'Seafarer Safety Monitor');
            app.UIFigure.CloseRequestFcn = @(~,~) app.onClose();

            app.StartButton = uibutton(app.UIFigure, 'push', ...
                'Text', 'Start Monitoring', 'Position', [30 590 150 30], ...
                'ButtonPushedFcn', @(~,e) app.StartButtonPushed(e));

            app.StopButton = uibutton(app.UIFigure, 'push', ...
                'Text', 'Stop Monitoring', 'Position', [195 590 150 30], ...
                'ButtonPushedFcn', @(~,e) app.StopButtonPushed(e));

            app.RiskStatusLabel = uilabel(app.UIFigure, ...
                'Text', 'Status', 'Position', [370 590 240 30], ...
                'FontSize', 18, 'FontWeight', 'bold');

            app.DetailLabel = uilabel(app.UIFigure, ...
                'Text', '', 'Position', [30 562 590 22], 'FontSize', 11);

            app.MEMSAxes       = uiaxes(app.UIFigure, 'Position', [40 380 570 170]);
            app.ECGAxes        = uiaxes(app.UIFigure, 'Position', [40 210 570 170]);
            app.WaterLevelAxes = uiaxes(app.UIFigure, 'Position', [40  40 570 170]);
        end

        function onClose(app)
            app.stopTimer();
            delete(app.UIFigure);
        end
    end
end
