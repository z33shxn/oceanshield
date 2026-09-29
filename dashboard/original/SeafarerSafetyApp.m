 classdef SeafarerSafetyApp < matlab.apps.AppBase

    % Properties that correspond to app components
    properties (Access = public)
        UIFigure matlab.ui.Figure
        StartButton matlab.ui.control.Button
        StopButton matlab.ui.control.Button
        RiskStatusLabel matlab.ui.control.Label
        MEMSAxes matlab.ui.control.UIAxes
        ECGAxes matlab.ui.control.UIAxes
        WaterLevelAxes matlab.ui.control.UIAxes
    end

    properties (Access = private)
        TimerObj timer
        ChannelID = 0     % your ThingSpeak channel ID
        ReadAPIKey = 'YOUR_THINGSPEAK_READ_KEY'
        NumPoints = 10
    end

    methods (Access = private)

        function updateData(app, ~, ~)

            % Read data from ThingSpeak
            try
                memsData = thingSpeakRead(app.ChannelID, ...
                    'Fields',1,'NumPoints',app.NumPoints,...
                    'ReadKey',app.ReadAPIKey);

                ecgData = thingSpeakRead(app.ChannelID, ...
                    'Fields',2,'NumPoints',app.NumPoints,...
                    'ReadKey',app.ReadAPIKey);

                waterLevelData = thingSpeakRead(app.ChannelID, ...
                    'Fields',3,'NumPoints',app.NumPoints,...
                    'ReadKey',app.ReadAPIKey);

            catch
                disp('Error reading from ThingSpeak');
                return;
            end

            % Risk calculation
            
                memsRisk = mean(memsData) > 2000;
                ecgRisk = mean(ecgData) > 120;
                waterRisk = any(waterLevelData > 1);
 
              riskScore = memsRisk + ecgRisk + waterRisk;

% Display values
disp(['MEMS: ', num2str(memsRisk), ...
      ' ECG: ', num2str(ecgRisk), ...
      ' WATER: ', num2str(waterRisk), ...
      ' TOTAL: ', num2str(riskScore)]);
           % disp(['MEMS:', num2str(memsRisk), ' ECG:', num2str(ecgRisk), ' WATER:', num2str(waterRisk)])
           

            % Update status
            if riskScore == 1
                app.RiskStatusLabel.Text = 'Safe';
                app.RiskStatusLabel.FontColor = [0 0.5 0];
   
          elseif riskScore == 2
                app.RiskStatusLabel.Text = 'Moderate Risk';
                app.RiskStatusLabel.FontColor = [0.9 0.6 0];

            else
                app.RiskStatusLabel.Text = 'High Risk';
                app.RiskStatusLabel.FontColor = [1 0 0];
            end

            % Plot data
            plot(app.MEMSAxes, memsData, '-b', 'LineWidth', 1.5);
            title(app.MEMSAxes, 'MEMS Sensor Data');
            grid(app.MEMSAxes, 'on');
            xlabel(app.MEMSAxes, 'Time (Samples)');
            ylabel(app.MEMSAxes, 'Acceleration (Raw Units)');

            plot(app.ECGAxes, ecgData, '-r', 'LineWidth', 1.5);
            title(app.ECGAxes, 'ECG Sensor Data');
            grid(app.ECGAxes, 'on');
            xlabel(app.ECGAxes, 'Time (Samples)');
            ylabel(app.ECGAxes, 'ECG Signal Amplitude (Raw)');

            plot(app.WaterLevelAxes, waterLevelData, '-k', 'LineWidth', 1.5);
            title(app.WaterLevelAxes, 'Water Level Sensor');
            grid(app.WaterLevelAxes, 'on');
            xlabel(app.WaterLevelAxes, 'Time (Samples)');
            ylabel(app.WaterLevelAxes, 'Water Presence');
        end

        % Start button
        function StartButtonPushed(app, event)
            app.TimerObj = timer( ...
                'ExecutionMode','fixedSpacing', ...
                'Period',5, ...
                'TimerFcn',@(~,~) app.updateData);
            start(app.TimerObj);
        end

        % Stop button
        function StopButtonPushed(app, event)
            if ~isempty(app.TimerObj)
                stop(app.TimerObj);
                delete(app.TimerObj);
                app.TimerObj = [];
            end
        end
    end

    methods (Access = public)

        % Construct app
        function app = SeafarerSafetyApp

            app.UIFigure = uifigure( ...
                'Position',[100 100 600 600], ...
                'Name','Seafarer Safety Monitor');

            app.StartButton = uibutton(app.UIFigure,'push', ...
                'Text','Start Monitoring', ...
                'Position',[50 550 150 30], ...
                'ButtonPushedFcn',@(btn,event) StartButtonPushed(app,event));

            app.StopButton = uibutton(app.UIFigure,'push', ...
                'Text','Stop Monitoring', ...
                'Position',[220 550 150 30], ...
                'ButtonPushedFcn',@(btn,event) StopButtonPushed(app,event));

            app.RiskStatusLabel = uilabel(app.UIFigure, ...
                'Text','Status', ...
                'Position',[400 550 150 30], ...
                'FontSize',16, ...
                'FontWeight','bold');

            app.MEMSAxes = uiaxes(app.UIFigure,'Position',[50 350 500 150]);
            app.ECGAxes = uiaxes(app.UIFigure,'Position',[50 200 500 150]);
            app.WaterLevelAxes = uiaxes(app.UIFigure,'Position',[50 50 500 150]);
        end
    end
end