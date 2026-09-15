package com.example.duckusb;

interface IDuckService {
    int getVersion();
    Bundle getState();
    void pushConfig(in Bundle config);
    List<Bundle> getRecords();
    void clearRecords();
}
