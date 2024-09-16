// ============================================================================
// INCLUDES
// ============================================================================

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <time.h>
#endif

// ============================================================================
// CONFIG
// ============================================================================

// how often to print stats
#define STATS_INTERVAL_MS 5000

// default values defined by qmk
#define QMK_RAW_HID_USAGE_PAGE 0xFF60
#define QMK_RAW_HID_USAGE 0x61

// can be adjusted if necessary to avoid collisions with other things that use raw hid
#define RAW_HID_HUB_COMMAND_ID 0x27

// control speed of the main loop
#define USE_SLEEP_WINDOWS
#define USE_SMART_SLEEP_WINDOWS 
#define SLEEP_MILLISECONDS_WINDOWS 1  // actual sleep duration depends on timer precision
#define SMART_SLEEP_WAIT_MILLISECONDS_WINDOWS 200
#define USE_SLEEP_POSIX
#define USE_SMART_SLEEP_POSIX
#define SLEEP_MICROSECONDS_POSIX 4167
#define SMART_SLEEP_WAIT_MILLISECONDS_POSIX 200

// ============================================================================
// MACROS
// ============================================================================

// sometimes bools aren't available??
#define true 1
#define false 0

// qmk raw hid protocol
#define QMK_RAW_HID_REPORT_SIZE 32
#define QMK_RAW_HID_REPORT_ID 0x0

// custom raw hid hub protocol
#define N_UNIQUE_DEVICE_IDS 255  // 0-254 are for devices, 255 is reserved
#define DEVICE_ID_UNASSIGNED N_UNIQUE_DEVICE_IDS
#define DEVICE_ID_HUB N_UNIQUE_DEVICE_IDS
#define MAX_REGISTERED_DEVICES 30

#define DEVICE_ID_IS_VALID(device_id) (0 <= device_id && device_id < N_UNIQUE_DEVICE_IDS)

// ============================================================================
// HIDAPI
// ============================================================================

#include <hidapi.h>

#ifndef HID_API_MAKE_VERSION
#define HID_API_MAKE_VERSION(mj, mn, p) (((mj) << 24) | ((mn) << 8) | (p))
#endif
#ifndef HID_API_VERSION
#define HID_API_VERSION HID_API_MAKE_VERSION(HID_API_VERSION_MAJOR, HID_API_VERSION_MINOR, HID_API_VERSION_PATCH)
#endif

#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_darwin.h>
#endif

#if defined(_WIN32) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_winapi.h>
#endif

#if defined(USING_HIDAPI_LIBUSB) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_libusb.h>
#endif

// ============================================================================
// TYPEDEFS
// ============================================================================

typedef struct raw_hid_node_t {
    hid_device* device;
    char* path;
    int device_id;
    int is_in_enumeration;
    struct raw_hid_node_t* next;
} raw_hid_node_t;

typedef struct raw_hid_message_t {
    unsigned char data[QMK_RAW_HID_REPORT_SIZE];
    struct raw_hid_message_t* next;
} raw_hid_message_t;

typedef struct raw_hid_message_counter_t {
    unsigned char origin_device_id;
    unsigned char destination_device_id;
    int count;
    struct raw_hid_message_counter_t* next;
} raw_hid_message_counter_t;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

int verbose = false;
raw_hid_node_t* raw_hid_nodes = NULL;  // root of the linked list of opened devices
int n_registered_devices = 0;
int next_unassigned_device_id = 1;
raw_hid_message_t* device_id_message_queue[N_UNIQUE_DEVICE_IDS];  // roots of linked lists of outgoing messages
raw_hid_message_counter_t* message_counters = NULL;
uint64_t iters_since_last_stats = 0;

// initialized in main
int device_id_is_assigned[N_UNIQUE_DEVICE_IDS];
unsigned char assigned_device_ids[MAX_REGISTERED_DEVICES];
unsigned char buffer_report_id_and_data[QMK_RAW_HID_REPORT_SIZE + 1];
unsigned char* buffer_data;
uint64_t current_time_ms;
uint64_t last_stats_time_ms;
uint64_t last_message_time_ms;

// ============================================================================
// TIME
// ============================================================================

void update_current_time_ms() {
#ifdef _WIN32
    current_time_ms = (uint64_t)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    current_time_ms = (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;
#endif
}

// ============================================================================
// VERBOSE UTILITIES
// ============================================================================

void message_counter_increment(unsigned char origin_device_id, unsigned char destination_device_id) {
    raw_hid_message_counter_t* current_counter = message_counters;
    raw_hid_message_counter_t* previous_counter = NULL;
    while (current_counter != NULL) {
        if (current_counter->origin_device_id == origin_device_id && current_counter->destination_device_id == destination_device_id) {
            current_counter->count++;
            return;
        }
        previous_counter = current_counter;
        current_counter = current_counter->next;
    }
    raw_hid_message_counter_t* new_counter = (raw_hid_message_counter_t*)malloc(sizeof(raw_hid_message_counter_t));
    if (new_counter == NULL) {
        return;
    }
    new_counter->origin_device_id = origin_device_id;
    new_counter->destination_device_id = destination_device_id;
    new_counter->count = 1;
    new_counter->next = NULL;
    if (message_counters == NULL) {
        message_counters = new_counter;
    } else {
        previous_counter->next = new_counter;
    }
}

void message_counter_free_all(void) {
    raw_hid_message_counter_t* current_counter = message_counters;
    raw_hid_message_counter_t* next_counter = NULL;
    while (current_counter != NULL) {
        next_counter = current_counter->next;
        free(current_counter);
        current_counter = next_counter;
    }
    message_counters = NULL;
}

void maybe_print_and_update_stats() {
    if (verbose < 2) {
        return;
    }
    iters_since_last_stats++;
    uint64_t delta_time_ms = current_time_ms - last_stats_time_ms;
    if (delta_time_ms < STATS_INTERVAL_MS) {
        return;
    }
    float delta_time_seconds = delta_time_ms / 1000.0;
    raw_hid_message_counter_t* current_counter = message_counters;
    printf("Main loop ran %lld times (%.2f per second).\n", iters_since_last_stats, iters_since_last_stats / delta_time_seconds);
    printf("Message counts:\n");
    while (current_counter != NULL) {
        printf("  [0x%02hx -> 0x%02hx]: %4d (%7.2f per second).\n", current_counter->origin_device_id, current_counter->destination_device_id, current_counter->count, current_counter->count / delta_time_seconds);
        current_counter = current_counter->next;
    }
    message_counter_free_all();
    last_stats_time_ms = current_time_ms;
    iters_since_last_stats = 0;
}

void print_device_info(struct hid_device_info* device) {
    printf("  Path:         %s\n", 		device->path);
    printf("  Manufacturer: %ls\n", 	device->manufacturer_string);
    printf("  Product:      %ls\n", 	device->product_string);
    printf("  Serial:       %ls\n", 	device->serial_number);
    printf("  Release:      %hx\n", 	device->release_number);
    printf("  Vendor ID:    0x%04hx\n",	device->vendor_id);
    printf("  Product ID:   0x%04hx\n", device->product_id);
    printf("  Usage Page:   0x%04hx\n", device->usage_page);
    printf("  Usage:        0x%02hx\n", device->usage);
}

void print_buffer(void) {
    for (size_t i = 0; i < QMK_RAW_HID_REPORT_SIZE; i++) {
        printf("%02X ", buffer_data[i]);
    }
    printf("\n");
}

// ============================================================================
// raw_hid_node_t MEMORY MANAGEMENT
// ============================================================================

raw_hid_node_t* raw_hid_node_new(hid_device* device, const char* path) {
    raw_hid_node_t* new_node = (raw_hid_node_t*)malloc(sizeof(raw_hid_node_t));
    if (new_node == NULL) {
        return NULL;
    }
    new_node->device = device;
    new_node->path = strdup(path);
    if (new_node->path == NULL) {
        free(new_node);
        return NULL;
    }
    new_node->device_id = DEVICE_ID_UNASSIGNED;
    new_node->is_in_enumeration = false;
    new_node->next = raw_hid_nodes;
    raw_hid_nodes = new_node;
    return new_node;
}

void raw_hid_node_free(raw_hid_node_t* node) {
    if (node == NULL) {
        return;
    }
    hid_close(node->device);
    free(node->path);
    free(node);
}

void raw_hid_node_free_all(void) {
    raw_hid_node_t* current_node = raw_hid_nodes;
    raw_hid_node_t* next_node = NULL;
    while (current_node != NULL) {
        next_node = current_node->next;
        raw_hid_node_free(current_node);
        current_node = next_node;
    }
    raw_hid_nodes = NULL;
}

// ============================================================================
// raw_hid_message_t MEMORY MANAGEMENT
// ============================================================================

void message_queue_push(int device_id, const unsigned char* data) {
    if (!DEVICE_ID_IS_VALID(device_id)) {
        return;
    }
    raw_hid_message_t* new_message = (raw_hid_message_t*)malloc(sizeof(raw_hid_message_t));
    if (new_message == NULL) {
        return;
    }
    memcpy(new_message->data, data, QMK_RAW_HID_REPORT_SIZE);
    new_message->next = NULL;
    if (device_id_message_queue[device_id] == NULL) {
        device_id_message_queue[device_id] = new_message;
    } else {
        raw_hid_message_t* current_message = device_id_message_queue[device_id];
        while (current_message->next != NULL) {
            current_message = current_message->next;
        }
        current_message->next = new_message;
    }
}

void message_queue_pop(int device_id, unsigned char* buffer) {
    if (!DEVICE_ID_IS_VALID(device_id)) {
        return;
    }
    if (device_id_message_queue[device_id] == NULL) {
        return;
    }
    raw_hid_message_t* popped_message = device_id_message_queue[device_id];
    memcpy(buffer, popped_message->data, QMK_RAW_HID_REPORT_SIZE);
    device_id_message_queue[device_id] = popped_message->next;
    free(popped_message);
}

void message_queue_clear(int device_id) {
    if (!DEVICE_ID_IS_VALID(device_id)) {
        return;
    }
    raw_hid_message_t* current_message = device_id_message_queue[device_id];
    raw_hid_message_t* previous_message = NULL;
    while (current_message != NULL) {
        previous_message = current_message;
        current_message = current_message->next;
        free(previous_message);
    }
    device_id_message_queue[device_id] = NULL;
}

void message_queue_clear_all(void) {
    for (int device_id = 0; device_id < N_UNIQUE_DEVICE_IDS; device_id++) {
        message_queue_clear(device_id);
    }
}

// ============================================================================
// DEVICE REGISTRATION/UNREGISTRATION
// ============================================================================

int register_node(raw_hid_node_t* node) {
    // returns 1 if the registration was successful, 0 if the node was already registered, -1 for error
    if (DEVICE_ID_IS_VALID(node->device_id)) {
        return 0;
    }
    if (n_registered_devices == MAX_REGISTERED_DEVICES) {
        if (verbose > 0) {
            printf("Too many registered devices.\n");
        }
        return -1;
    }
    node->device_id = next_unassigned_device_id;
    device_id_is_assigned[next_unassigned_device_id] = true;
    while (device_id_is_assigned[next_unassigned_device_id] == true) {
        next_unassigned_device_id = (next_unassigned_device_id + 1) % N_UNIQUE_DEVICE_IDS;
    }
    assigned_device_ids[n_registered_devices] = node->device_id;
    n_registered_devices += 1;
    if (verbose > 0) {
        printf("Device was registered with ID: 0x%02hx\n", node->device_id);
    }
    return 0;
}

void unregister_node(raw_hid_node_t* node) {
    if (node->device_id == DEVICE_ID_UNASSIGNED) {
        return;
    }
    if (verbose > 0) {
        printf("Device with ID 0x%02hx was unregistered.\n", node->device_id);
    }
    message_queue_clear(node->device_id);
    for (int i = 0; i < n_registered_devices; i++) {
        if (assigned_device_ids[i] == node->device_id) {
            assigned_device_ids[i] = assigned_device_ids[n_registered_devices - 1];
            assigned_device_ids[n_registered_devices - 1] = DEVICE_ID_UNASSIGNED;
            break;
        }
    }
    node->device_id = DEVICE_ID_UNASSIGNED;
    device_id_is_assigned[node->device_id] = false;
    n_registered_devices -= 1;
}

// ============================================================================
// HID ENUMERATION
// ============================================================================

int handle_raw_hid_device_found(const char* path) {
    // returns 1 if a new device was opened, 0 if an existing open device was found, -1 for error
    raw_hid_node_t* current_node = raw_hid_nodes;
    while (current_node != NULL) {
        if (strcmp(current_node->path, path) == 0) {
            current_node->is_in_enumeration = true;
            return 0;
        }
        current_node = current_node->next;
    }
    hid_device* device = hid_open_path(path);
    if (device == NULL) {
        return -1;
    }
    hid_set_nonblocking(device, 1);  // set hid_read() to be nonblocking
    raw_hid_node_t* new_node = raw_hid_node_new(device, path);
    if (new_node == NULL) {
        hid_close(device);
        return -1;
    }
    new_node->is_in_enumeration = true;
    return 1;
}

void handle_raw_hid_device_missing(const char* path) {
    raw_hid_node_t* current_node = raw_hid_nodes;
    raw_hid_node_t* previous_node = NULL;
    while (current_node != NULL) {
        if (strcmp(current_node->path, path) == 0) {
            if (previous_node != NULL) {
                previous_node->next = current_node->next;
            } else {
                raw_hid_nodes = current_node->next;
            }
            unregister_node(current_node);
            raw_hid_node_free(current_node);
            return;
        }
        previous_node = current_node;
        current_node = current_node->next;
    }
}

void enumerate_raw_hid_devices(void) {

    // unmark existing open devices
    raw_hid_node_t* current_node = raw_hid_nodes;
    while (current_node != NULL) {
        current_node->is_in_enumeration = false;
        current_node = current_node->next;
    }
    
    // open any newly found devices
    struct hid_device_info* hid_enumeration_start = hid_enumerate(0x0, 0x0);
    struct hid_device_info* current_device_info = hid_enumeration_start;
    int result = 0;
    while (current_device_info != NULL) {
        if (current_device_info->usage_page == QMK_RAW_HID_USAGE_PAGE && current_device_info->usage == QMK_RAW_HID_USAGE) {
            result = handle_raw_hid_device_found(current_device_info->path);	
            if (verbose > 0 && result == 1) {
                printf("Opened a new raw HID device:\n");
                print_device_info(current_device_info);
            }
        }
        current_device_info = current_device_info->next;
    }
    hid_free_enumeration(hid_enumeration_start);

    // close devices that weren't found in the enumeration
    current_node = raw_hid_nodes;
    while (current_node != NULL) {
        if (current_node->is_in_enumeration == false) {
            handle_raw_hid_device_missing(current_node->path);
            if (verbose > 0) {
                printf("Closed a missing raw HID device.\n");
            }
        }
        current_node = current_node->next;
    }
}

// ============================================================================
// ACTUAL COMMUNICATION
// ============================================================================

void communicate_with_raw_hid_device(raw_hid_node_t* node) {

    // read from device
    int bytes_read = hid_read(node->device, buffer_data, QMK_RAW_HID_REPORT_SIZE);
    int result;
    unsigned char destination_device_id;
    while (bytes_read > 0) {
        if (buffer_data[0] != RAW_HID_HUB_COMMAND_ID) {
            if (verbose > 3) {
                printf("Discarding:          ");
                print_buffer();
            }
            goto next_hid_read;
        } else {
            if (verbose > 2) {
                printf("Receiving from 0x%02hx: ", node->device_id);
                print_buffer();
            }

            // registration report
            if (buffer_data[1] == DEVICE_ID_HUB && buffer_data[2] == 0x01) {
                if (verbose > 1) {
                    message_counter_increment(node->device_id, DEVICE_ID_HUB);
                }
                result = register_node(node);
                if (result == 1) {
                    for (int i = 0; i < n_registered_devices; i++) {
                        destination_device_id = assigned_device_ids[i];
                        memcpy(buffer_data + 2, assigned_device_ids, MAX_REGISTERED_DEVICES);
                        for (int j = 3; j < n_registered_devices + 2; j++) {
                            if (buffer_data[j] == destination_device_id) {
                                buffer_data[j] = buffer_data[2];
                                buffer_data[2] = destination_device_id;
                                break;
                            }
                        }
                        message_queue_push(destination_device_id, buffer_data);
                        if (verbose > 1) {
                            message_counter_increment(DEVICE_ID_HUB, destination_device_id);
                        }
                    }
                } else if (result == 0) {
                    destination_device_id = node->device_id;
                    memcpy(buffer_data + 2, assigned_device_ids, MAX_REGISTERED_DEVICES);
                    for (int j = 3; j < n_registered_devices + 2; j++) {
                        if (buffer_data[j] == destination_device_id) {
                            buffer_data[j] = buffer_data[2];
                            buffer_data[2] = destination_device_id;
                            break;
                        }
                    }
                    message_queue_push(destination_device_id, buffer_data);
                    if (verbose > 1) {
                        message_counter_increment(DEVICE_ID_HUB, destination_device_id);
                    }
                }
                goto next_hid_read;
            }

            // remaining cases only apply to registered devices
            if (!DEVICE_ID_IS_VALID(node->device_id)) {
                goto next_hid_read;
            }

            // unregistration report
            if (buffer_data[1] == DEVICE_ID_HUB && buffer_data[2] == 0x00) {
                if (verbose > 1) {
                    message_counter_increment(node->device_id, DEVICE_ID_HUB);
                }
                unregister_node(node);
                for (int i = 0; i < n_registered_devices; i++) {
                    destination_device_id = assigned_device_ids[i];
                    memcpy(buffer_data + 2, assigned_device_ids, MAX_REGISTERED_DEVICES);
                    for (int j = 3; j < n_registered_devices + 2; j++) {
                        if (buffer_data[j] == destination_device_id) {
                            buffer_data[j] = buffer_data[2];
                            buffer_data[2] = destination_device_id;
                            break;
                        }
                    }
                    message_queue_push(destination_device_id, buffer_data);
                    if (verbose > 1) {
                        message_counter_increment(DEVICE_ID_HUB, destination_device_id);
                    }
                }
                goto next_hid_read;
            }

            // message report
            if (buffer_data[1] != DEVICE_ID_HUB) {
                destination_device_id = buffer_data[1]; 
                if (device_id_is_assigned[destination_device_id] == false) {
                    goto next_hid_read;
                }
                buffer_data[1] = node->device_id;
                message_queue_push(destination_device_id, buffer_data);
                if (verbose > 1) {
                    message_counter_increment(node->device_id, destination_device_id);
                }
#if (defined(_WIN32) && defined(USE_SMART_SLEEP_WINDOWS)) || (!defined(_WIN32) && defined(USE_SMART_SLEEP_POSIX))
                last_message_time_ms = current_time_ms;
#endif
                goto next_hid_read;
            }

next_hid_read:
        bytes_read = hid_read(node->device, buffer_data, QMK_RAW_HID_REPORT_SIZE);

        }
    }

    // send to device
    while (device_id_message_queue[node->device_id] != NULL) {
        message_queue_pop(node->device_id, buffer_data);
        if (verbose > 2) {
            printf("Sending to 0x%02hx:     ", node->device_id);
            print_buffer();
        }
        hid_write(node->device, buffer_report_id_and_data, QMK_RAW_HID_REPORT_SIZE + 1);
    }
}

void send_hub_shutdown_reports(void) {
    buffer_data[0] = RAW_HID_HUB_COMMAND_ID;
    buffer_data[1] = DEVICE_ID_HUB;
    buffer_data[2] = DEVICE_ID_UNASSIGNED;
    raw_hid_node_t* current_node = raw_hid_nodes;
    while (current_node != NULL) {
        if (DEVICE_ID_IS_VALID(current_node->device_id)) {
            hid_write(current_node->device, buffer_report_id_and_data, QMK_RAW_HID_REPORT_SIZE + 1);
        }
        current_node = current_node->next;
    }
}

// ============================================================================
// MAIN
// ============================================================================

void cleanup() {
    send_hub_shutdown_reports();
    raw_hid_node_free_all();
    message_queue_clear_all();
    message_counter_free_all();
    hid_exit();
    if (verbose > 0) {
        printf("Cleanup completed.\n");
    }
}

void signal_handler(int signal) {
    cleanup();
    exit(signal);
}

int main(int argc, char* argv[])
{
    // crude parser for verbose argument
    if (argc > 1 && strncmp(argv[1], "-v", 2) == 0) {
        verbose = argv[1][2] - '0';
    }

    // register signal handler for termination signals
    signal(SIGINT, signal_handler);   // Handles Ctrl+C
    signal(SIGTERM, signal_handler);  // Handles termination request
    signal(SIGABRT, signal_handler);  // Handles abort signals

    // initialize hidapi
    int result = hid_init();
    if (result == -1)
        // print statement causes initialization to fail?!
        // printf("HIDAPI initialization unsuccessful.\n");
        return -1;
#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
    hid_darwin_set_open_exclusive(0);
#endif
    if (verbose > 0) {
        printf("HIDAPI initialization successful.\n");
    }

    // initialize global variables
    memset(device_id_is_assigned, false, sizeof(device_id_is_assigned));
    memset(assigned_device_ids, DEVICE_ID_UNASSIGNED, sizeof(assigned_device_ids));
    memset(buffer_report_id_and_data, 0, sizeof(buffer_report_id_and_data));
    buffer_report_id_and_data[0] = QMK_RAW_HID_REPORT_ID;
    buffer_data = buffer_report_id_and_data + 1;
    update_current_time_ms();
    last_stats_time_ms = current_time_ms;
    last_message_time_ms = current_time_ms;

    // enumerate once on startup (TODO: perform periodic enumerations in another thread)
    enumerate_raw_hid_devices();

    // main loop
    while (true) {

        // update time
        update_current_time_ms();

        // actual hid task
        raw_hid_node_t* current_node = raw_hid_nodes;
        while (current_node != NULL) {
            communicate_with_raw_hid_device(current_node);
            current_node = current_node->next;
        }

        // print stats
        maybe_print_and_update_stats();

        // sleep to reduce resource usage
#if defined(USE_SLEEP_WINDOWS) && defined(_WIN32)
#    ifdef USE_SMART_SLEEP_WINDOWS
        if (last_message_time_ms - current_time_ms > SMART_SLEEP_WAIT_MILLISECONDS_WINDOWS) {
            Sleep(SLEEP_MILLISECONDS_WINDOWS);
        }
#    else
        Sleep(SLEEP_MILLISECONDS_WINDOWS);
#    endif
#elif defined(USE_SLEEP_POSIX) && !defined(_WIN32)
#    ifdef USE_SMART_SLEEP_POSIX
        if (last_message_time_ms - current_time_ms > SMART_SLEEP_WAIT_MILLISECONDS_WINDOWS) {
            usleep(SLEEP_MICROSECONDS_POSIX);
        }
#    else
        usleep(SLEEP_MICROSECONDS_POSIX);
#    endif
#endif

    }
    
    // cleanup
    cleanup();
    return 0;
}