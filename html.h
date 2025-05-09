/**
 * @file  html.h
 * @brief HTML templates
 */

// Static HTML template
const char htmlHeader[] = R"EOF(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta http-equiv="X-UA-Compatible" content="ie=edge">
    <meta http-equiv="refresh" content="60">
    <title>%s</title>
    <style>
      table.sensor {
        border-top: solid 1px black;
        border-left: solid 1px black;
        margin-left: auto;
        margin-right: auto;
      }
      table.sensor th, table.sensor td {
        border-right: solid 1px black;
        border-bottom: solid 1px black;
      }
      table.sensor th.header {
        background-color: #0000CC;
        color: white;
        font-size: 24px;
        font-weight: bold;
        padding: 5px 10px;
        text-align: center;
      }
      table.sensor th {
        text-align: left;
        padding-right: 50px;
      }
      table.sensor td {
        padding-left: 50px;
        text-align: right;
      }
      table.sensor tr.light {
        background-color: #E8E8FF;
      }
      table.sensor tr.environmental {
        background-color: #D0D0FF;
      }
      table.sensor tr.system {
        background-color: #B8B8FF;
      }
      table.sensor tr.chip {
        background-color: #A0A0FF;
      }
      a, a:visited {
        color: blue;
      }
      a:hover {
        color: purple;
      }
      div.chartContainer {
        width: 1500px;
        height: 475px;
        margin-top: 40px;
        margin-left: auto;
        margin-right: auto;
        text-align: center;
      }
      canvas.chart {
        width: 1500px;
        height: 475px;
      }
    </style>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <script>
      var jsonData;
      var esp32Time = %lld;
      var tempUnits = '%s';

      var sensorChartData = {
        datasets: [
          {
            label: 'Sound (dB)',
            borderColor: 'blue',
            backgroundColor: 'blue',
            yAxisID: 'yS',
            pointRadius: 1
          },
          {
            label: 'Light (lux)',
            borderColor: 'cyan',
            backgroundColor: 'cyan',
            yAxisID: 'yL',
            pointRadius: 1
          }
        ]
      };

      var sensorChartOptions = {
        type: 'line',
        options: {
          responsive: true,
          maintainAspectRatio: false,
          interaction: {
            mode: 'index',
            intersect: false,
          },
          stacked: false,
          plugins: {
            title: {
              display: false,
              text: 'Sound and Light'
            }
          },
          scales: {
            yS: {
              type: 'linear',
              display: true,
              position: 'left',
              ticks: { color: 'blue' }
            },
            yL: {
              type: 'logarithmic',
              display: true,
              position: 'right',
              ticks: { color: 'cyan' },
              grid: {
                drawOnChartArea: false // Only want grid lines for one axis to show up
              }
            }
          }
        },
      };

      var environmentalChartData = {
        datasets: [
          {
            label: 'Pressure (mbar)',
            borderColor: 'blue',
            backgroundColor: 'blue',
            yAxisID: 'yP',
            pointRadius: 1
          },
          {
            label: 'Temperature (' + tempUnits + ')',
            borderColor: 'red',
            backgroundColor: 'red',
            yAxisID: 'yT',
            pointRadius: 1
          },
          {
            label: 'Dew Point (' + tempUnits + ')',
            borderColor: '#00CC00',
            backgroundColor: '#00CC00',
            yAxisID: 'yD', // Change to 'yT' to share the Temperature axis, then remove the 'yD' definition in 'scales' below
            pointRadius: 1,
            hidden: true // Initially hide the dew point graph to keep the chart from looking too busy
          },
          {
            label: 'Humidity (%%)', // Need double percent symbols because of sprintf() in the C code
            borderColor: 'green',
            backgroundColor: 'green',
            yAxisID: 'yH',
            pointRadius: 1
          }
        ]
      };

      var environmentalChartOptions = {
        type: 'line',
        options: {
          responsive: true,
          maintainAspectRatio: false,
          interaction: {
            mode: 'index',
            intersect: false,
          },
          stacked: false,
          plugins: {
            title: {
              display: false,
              text: 'Environmentals'
            }
          },
          scales: {
            yP: {
              type: 'linear',
              display: true,
              position: 'left',
              ticks: { color: 'blue' },
              grid: {
                drawOnChartArea: false // Only want grid lines for one axis to show up
              }
            },
            yT: {
              type: 'linear',
              display: true,
              position: 'right',
              ticks: { color: 'red' }
            },
            yD: {
              type: 'linear',
              display: true,
              position: 'right',
              ticks: { color: '#00CC00' },
              grid: {
                drawOnChartArea: false // Only want grid lines for one axis to show up
              }
            },
            yH: {
              type: 'linear',
              display: true,
              position: 'right',
              ticks: { color: 'green' },
              grid: {
                drawOnChartArea: false // Only want grid lines for one axis to show up
              }
            }
          }
        },
      };

      document.addEventListener('DOMContentLoaded', function() {
        fetch('/data')
        .then(response => {
            if (response.ok) return response.text(); // The data is in plain text and not JSON
        })
        .then(data => {
          jsonData = JSON.parse('[' + data.trim().slice(0,-1) + ']'); // slice() removes the trailing comma
          if (jsonData.length)
          {
            var streamLength = jsonData.length / 6; // There are six data streams
            var sound        = jsonData.slice(0               , streamLength);
            var light        = jsonData.slice(streamLength    , streamLength * 2);
            var temperature  = jsonData.slice(streamLength * 2, streamLength * 3); // Always in Celsius
            var humidity     = jsonData.slice(streamLength * 3, streamLength * 4);
            var pressure     = jsonData.slice(streamLength * 4, streamLength * 5);
            var timeIndex    = jsonData.slice(streamLength * 5, streamLength * 6);

            // Convert the ESP32 timestamps to local time in the browser
            var days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
            var now = Date.now(); // ms
            for (var i = 0; i < timeIndex.length; i++)
            {
              var date = new Date(now - (esp32Time - timeIndex[i]) * 1000);
              timeIndex[i] = days[date.getDay()] + ' ' + date.toLocaleDateString().slice(0,-5) + ' ' + date.toLocaleTimeString();
            }

            sensorChartData.labels = timeIndex;
            sensorChartData.datasets[0].data = sound;
            sensorChartData.datasets[1].data = light;
            sensorChartOptions.data = sensorChartData;
            new Chart(document.getElementById('chartSoundLight'), sensorChartOptions);

            // Calculate dew point and convert temperature[]  to Fahrenheit if needed
            var dewPoint = [];
            for (var i = 0; i < temperature.length; i++)
            {
              var t = temperature[i]; // Celsius
              var magnusGammaTRH = Math.log(humidity[i] / 100.0) + 17.625 * t / (243.04 + t);
              var d = 243.04 * magnusGammaTRH / (17.625 - magnusGammaTRH);
              if (tempUnits == 'F')
              {
                d = d * 9.0 / 5.0 + 32.0;
                temperature[i] = t * 9.0 / 5.0 + 32.0;
              }
              dewPoint.push(d);
            }

            environmentalChartData.labels = timeIndex;
            environmentalChartData.datasets[0].data = pressure;
            environmentalChartData.datasets[1].data = temperature;
            environmentalChartData.datasets[2].data = dewPoint;
            environmentalChartData.datasets[3].data = humidity;
            environmentalChartOptions.data = environmentalChartData;
            new Chart(document.getElementById('chartEnvironmentals'), environmentalChartOptions);
          }
        });
      });
    </script>
  </head>
  <body>
)EOF";
const char htmlFooter[] = R"EOF(
    <div class="chartContainer">Click on captions to enable/disable graphs<br/><canvas class="chart" id="chartSoundLight"></canvas></div>
    <div class="chartContainer">Click on captions to enable/disable graphs<br/><canvas class="chart" id="chartEnvironmentals"></canvas></div>
  </body>
</html>
)EOF";
