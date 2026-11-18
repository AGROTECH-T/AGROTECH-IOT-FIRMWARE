#ifndef AGROTECH_COORDINATOR_SENSORS_H
#define AGROTECH_COORDINATOR_SENSORS_H

#include <stdint.h>

#include "AgrotechProtocol.h"
#include "CoordinatorSensorMath.h"

namespace coordinator {

enum class SensorSource : uint8_t { None, Bme680, Dht11 };

}  // namespace coordinator

void initializeSensors();
// Runs one acquisition cycle of the failover state machine and fills `fields`
// with the published pair (BME680 on channels 1/2, DHT11 on channels 3/4).
// Returns the published source; None means no frame must be generated.
coordinator::SensorSource readConfiguredSensors(agrotech::FrameFields* fields);
// Functional failover state decided by the last readConfiguredSensors() call
// (PRINCIPAL before the first cycle, decision D-1). Drives the LED.
coordinator_sensor::FailoverState currentFailoverState();

#endif
