/**
 * @file  MeasurementTracker.h
 * @brief Helper class to track sensor measurements with min, average and max calculations
 */

#ifndef __MEASUREMENT_TRACKER_H__
#define __MEASUREMENT_TRACKER_H__

class MeasurementTracker
{
  private:
    float* data; // Array of data points
    int dataSize = 0; // Number of data points
    int cursor = 0; // Current index into the data array
    bool dataFull = false; // Set to true when the cursor wraps around and the array is full of data

  public:
    float current; // Current value for the metric, which is also at data[cursor]
    float min, max, average; // Statistics about the data in the array, updated each time new data points are added

    // Constructor
    MeasurementTracker(int dataArraySize)
    {
      // Allocate memory for data array
      data = new float[dataArraySize];
      dataSize = dataArraySize;
      dataFull = false;
      cursor = 0;
    }

    // Destructor
    ~MeasurementTracker()
    {
      // Free memory
      delete[] data;
      data = nullptr;
      dataSize = 0;
    }

    // Track a new data point and recompute min/max/average values
    void track(float dataPoint)
    {
      // Capture the current value
      current = dataPoint;

      // Add a new data point to the tracking array
      data[cursor] = dataPoint;
      cursor++;
      if (cursor >= dataSize)
      {
        cursor = 0; // Wrap around
        dataFull = true;
      }

      // Compute min/max/average values from the data array
      int j = dataFull ? dataSize : cursor;
      min = data[0];
      max = data[0];
      double sum = data[0];
      for (int i = 1; i < j; i++)
      {
        float d = data[i];
        if (d < min) min = d;
        if (d > max) max = d;
        sum += d;
      }
      average = (float)(sum / (double)j);
    }
};

#endif
