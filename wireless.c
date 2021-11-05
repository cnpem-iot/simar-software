#include <dirent.h>
#include <hiredis/hiredis.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "BME280/common/common.h"
#include "SPI/common.h"

redisContext *c, *local_c;
const char servers[3][11] = {"10.0.38.46", "10.0.38.59", "10.0.38.42"};
gpio_t led = {.pin = USR_3};
int8_t sensor_number = -1;

void* blink_led() {
  const struct timespec blink_delay = {0, 250000000L};
  const struct timespec blink_period = {0, 750000000L};
  while (1) {
    for (int i = 0; i < sensor_number; i++) {
      bbb_mmio_set_high(led);
      nanosleep(&blink_delay, NULL);
      bbb_mmio_set_low(led);
      nanosleep(&blink_delay, NULL);
    }
    nanosleep(&blink_period, NULL);
  }
}

/**
 * @brief Checks if the alteration in pressure over one measurement is realistic
 *
 * @param[in] sensor : Sensor to check
 *
 * @return void
 */
int8_t check_alteration(struct sensor_data sensor) {
  if (sensor.data.pressure == 1100 || sensor.data.pressure < 1) {
    syslog(LOG_CRIT,
           "Sensor %s failed to respond with valid measurements, unlikely to "
           "recover, reinitializing all sensors",
           sensor.name);
    exit(-3);
  }

  return sensor.past_pres == 0 ||
         (fabs(sensor.past_pres - sensor.data.pressure) < sensor.past_pres / 7 &&
          sensor.data.pressure > 800 && sensor.data.pressure < 1000 && sensor.data.humidity != 100);
}

int main(int argc, char* argv[]) {
  openlog("simar_bme", 0, LOG_LOCAL0);

  redisReply* reply;
  struct sensor_data sensor;
  syslog(LOG_NOTICE, "Starting up...");

  sensor.dev.settings.osr_h = BME280_OVERSAMPLING_4X;
  sensor.dev.settings.osr_p = BME280_OVERSAMPLING_16X;
  sensor.dev.settings.osr_t = BME280_OVERSAMPLING_4X;
  sensor.dev.settings.filter = BME280_FILTER_COEFF_OFF;
  sensor.id.mux_id = 0;
  sensor.past_pres = 0;

  if (bme_init(&sensor.dev, &sensor.id, 0x76) == BME280_OK) {
    sensor.dev.intf_ptr = &sensor.id;
  } else {
    syslog(LOG_CRIT, "No sensor found");
    return -2;
  }

  int server_i = 0;

  do {
    c = redisConnectWithTimeout(servers[server_i], 6379, (struct timeval){1, 500000});

    if (c->err) {
      if (server_i == 2) {
        syslog(LOG_ERR, "No remote Redis server instance found");
        return -3;
      }

      syslog(LOG_ERR, "%s remote Redis server not available, switching...\n", servers[server_i++]);
    }

  } while (c->err);

  redisSetTimeout(c, (struct timeval){1, 500000});

  syslog(LOG_NOTICE, "Redis DB connected");

  for (int i = 0; i < 20; i++) {
    local_c = redisConnectWithTimeout("127.0.0.1", 6379, (struct timeval){1, 500000});
    if (!local_c->err)
      break;

    sensor.dev.delay_us(500000, NULL);
  }

  if (local_c->err)
    syslog(LOG_CRIT, "Could not find a local Redis server");
  reply = (redisReply*)redisCommand(local_c, "HGET device simar_gia");

  if (reply->str) {
    int sensor_preassigned = atoi(reply->str);

    freeReplyObject(reply);
    reply = (redisReply*)redisCommand(c, "GET wgen%d_pressure", sensor_preassigned);

    if (!reply->str && sensor_preassigned < 9)
      sensor_number = sensor_preassigned;
    else
      syslog(LOG_NOTICE, "Preassigned SIMAR ID was not available, resorting to available ID");
  }
  freeReplyObject(reply);

  if (sensor_number == -1) {
    for (int i = 1; i < 9; i++) {
      reply = (redisReply*)redisCommand(c, "GET wgen%d_pressure", i);
      if (!reply->str) {
        sensor_number = i;
        freeReplyObject(reply);
        break;
      }
      freeReplyObject(reply);
    }

    if (sensor_number == -1) {
      syslog(LOG_CRIT, "Sensor could not be allocated a variable");
      exit(-3);
    }
  }

  syslog(LOG_NOTICE, "Sensor connected, utilizing id %d", sensor_number);
  reply = (redisReply*)redisCommand(local_c, "HSET device simar_gia %d", sensor_number);
  freeReplyObject(reply);

  reply = (redisReply*)redisCommand(local_c, "SET retries 0");
  freeReplyObject(reply);

  syslog(LOG_NOTICE, "Starting readings...");
  for (int i = 0; i < 10; i++)
    bme_read(&sensor.dev, &sensor.data);  // Perform "calibration" readings

  bbb_mmio_get_gpio(&led);
  bbb_mmio_set_output(led);

  pthread_t led_thread;
  pthread_create(&led_thread, NULL, blink_led, NULL);

  FILE* file = fopen("/log.csv", "a");
  char* filename;
  filename = (char*)calloc(512, sizeof(char));
  DIR* dr = opendir("/media/");
  struct dirent* de;
  int found_loc = 0;

  const struct timespec period = {0, 750000000L};

  time_t t = time(NULL);
  struct tm* current_time = localtime(&t);
  char time_str[64];

  if (dr == NULL) {
    syslog(LOG_ERR, "Could not open logging directory");
    return -4;
  }

  for (;;) {
    found_loc = 0;

    while ((de = readdir(dr)) != NULL) {
      if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
        if (strcmp(de->d_name, filename)) {
          snprintf(filename, 512, "/media/%s/datalog.csv", de->d_name);
          fclose(file);
          fopen(filename, "a");
        }
        found_loc = 1;
        break;
      }
    }

    if (!found_loc)
      strncpy(filename, "", 512);
    rewinddir(dr);

    bme_read(&sensor.dev, &sensor.data);
    if (check_alteration(sensor)) {
      reply = (redisReply*)redisCommand(c, "SET wgen%d_%s %.3f EX 5", sensor_number, "temperature",
                                        sensor.data.temperature);

      if (reply == NULL)
        return -2;
      freeReplyObject(reply);

      reply = (redisReply*)redisCommand(c, "SET wgen%d_%s %.3f EX 5", sensor_number, "pressure",
                                        sensor.data.pressure);
      freeReplyObject(reply);

      reply = (redisReply*)redisCommand(c, "SET wgen%d_%s %.3f EX 5", sensor_number, "humidity",
                                        sensor.data.humidity);
      freeReplyObject(reply);

      sensor.past_pres = sensor.data.pressure;
      if (strcmp(filename, "")) {
        t = time(0);
        current_time = localtime(&t);
        strftime(time_str, sizeof(time_str), "%c", current_time);

        fprintf(file, "%s,%.4f,%.4f,%.4f\n", time_str, sensor.data.temperature,
                sensor.data.pressure, sensor.data.humidity);
        fflush(file);
      }
    }
    nanosleep(&period, NULL);
  }

  // Unreachable
  free(filename);
  pthread_join(led_thread, NULL);
  redisFree(c);
  redisFree(local_c);
  closedir(dr);
  return 0;
}
