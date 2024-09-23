// ============================================================================
// INCLUDES
// ============================================================================

#ifndef _WIN32
#    define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#    include <process.h>
#    include <windows.h>
#else
#    include <errno.h>
#    include <pthread.h>
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
#define SMART_SLEEP_WAIT_MILLISECONDS_WINDOWS 100
#define USE_SLEEP_POSIX
#define USE_SMART_SLEEP_POSIX
#define SLEEP_MILLISECONDS_POSIX 4.16666667
#define SMART_SLEEP_WAIT_MILLISECONDS_POSIX 100

// control speed of the child loop
#define SECONDS_PER_ENUMERATION 1

// ============================================================================
// MACROS
// ============================================================================

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

#include <hidapi/hidapi.h>

#ifndef HID_API_MAKE_VERSION
#define HID_API_MAKE_VERSION(mj, mn, p) (((mj) << 24) | ((mn) << 8) | (p))
#endif
#ifndef HID_API_VERSION
#define HID_API_VERSION HID_API_MAKE_VERSION(HID_API_VERSION_MAJOR, HID_API_VERSION_MINOR, HID_API_VERSION_PATCH)
#endif

#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi/hidapi_darwin.h>
#endif

#if defined(_WIN32) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi/hidapi_winapi.h>
#endif

#if defined(USING_HIDAPI_LIBUSB) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi/hidapi_libusb.h>
#endif

// ============================================================================
// TYPEDEFS
// ============================================================================

typedef struct raw_hid_node_t {
    hid_device* device;
    char* path;
    int device_id;
    int is_in_enumeration;
    atomic_int is_marked_for_unregistration;  // only set by child
    atomic_int is_marked_for_deletion;  // only set by parent
    _Atomic(struct raw_hid_node_t*) next;  // only set by child
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

atomic_bool main_loop_new_iteration_flag = false;
atomic_bool child_termination_flag = false;

_Atomic(raw_hid_node_t*) raw_hid_nodes = NULL;  // only set by child
bool registrations_changed = false;
int n_registered_devices = 0;
int next_unassigned_device_id = 1;

// initialized in main
bool device_id_is_assigned[N_UNIQUE_DEVICE_IDS];
unsigned char assigned_device_ids[MAX_REGISTERED_DEVICES];
raw_hid_message_t* device_id_message_queue[N_UNIQUE_DEVICE_IDS];
unsigned char buffer_report_id_and_data[QMK_RAW_HID_REPORT_SIZE + 1];
unsigned char* buffer_data;
uint64_t current_time_ms;
uint64_t last_stats_time_ms;
uint64_t last_message_time_ms;

// only for verbose
bool verbose_basic = false;
bool verbose_stats = false;
bool verbose_hub = false;
bool verbose_device = false;
bool verbose_discard = false;
raw_hid_message_counter_t* message_counters = NULL;
uint64_t iters_since_last_stats = 0;

// ============================================================================
// TIME
// ============================================================================

void update_current_time_ms(void) {
#ifdef _WIN32
    current_time_ms = (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    current_time_ms = (uint64_t)(ts.tv_sec) * 1000 + (uint64_t)(ts.tv_nsec) / 1000000;
#endif
}

#ifndef _WIN32
void sleep_milliseconds(float milliseconds) {
    struct timespec req = {0};
    struct timespec rem = {0};
    req.tv_sec = (time_t)(milliseconds / 1000);
    req.tv_nsec = (long)((milliseconds - (req.tv_sec * 1000)) * 1e6);
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}
#endif

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
    if (!verbose_stats) {
        return;
    }
    iters_since_last_stats++;
    uint64_t delta_time_ms = current_time_ms - last_stats_time_ms;
    if (delta_time_ms < STATS_INTERVAL_MS) {
        return;
    }
    float delta_time_seconds = delta_time_ms / 1000.0;
    raw_hid_message_counter_t* current_counter = message_counters;
    printf("Main loop ran %llu times (%.2f per second).\n", (unsigned long long)iters_since_last_stats, iters_since_last_stats / delta_time_seconds);
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
// raw_hid_node_t MEMORY MANAGEMENT (child only)
// ============================================================================

raw_hid_node_t* raw_hid_node_new(hid_device* device, const char* path, raw_hid_node_t* previous_node) {
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
    new_node->is_in_enumeration = true;
    atomic_store(&(new_node->is_marked_for_unregistration), false);
    atomic_store(&(new_node->is_marked_for_deletion), false);
    atomic_store(&(new_node->next), NULL);
    if (previous_node == NULL) {
        atomic_store(&raw_hid_nodes, new_node);
    } else {
        atomic_store(&(previous_node->next), new_node);
    }
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
    raw_hid_node_t* current_node = atomic_load(&raw_hid_nodes);
    raw_hid_node_t* next_node = NULL;
    while (current_node != NULL) {
        next_node = atomic_load(&current_node->next);
        raw_hid_node_free(current_node);
        current_node = next_node;
    }
    atomic_store(&raw_hid_nodes, NULL);
}

// ============================================================================
// HID ENUMERATION (child only)
// ============================================================================

int handle_raw_hid_device_found(const char* path) {
    // returns 1 if a new device was opened, 0 if an existing open device was found, -1 for error
    raw_hid_node_t* current_node = atomic_load(&raw_hid_nodes);
    raw_hid_node_t* previous_node = NULL;
    while (current_node != NULL) {
        if ((strcmp(current_node->path, path) == 0) && (!atomic_load(&(current_node->is_marked_for_unregistration)))) {
            current_node->is_in_enumeration = true;
            return 0;
        }
        previous_node = current_node;
        current_node = atomic_load(&(current_node->next));
    }
    hid_device* device = hid_open_path(path);
    if (device == NULL) {
        return -1;
    }
    hid_set_nonblocking(device, 1);  // set hid_read() to be nonblocking
    raw_hid_node_t* new_node = raw_hid_node_new(device, path, previous_node);
    if (new_node == NULL) {
        hid_close(device);
        return -1;
    }
    return 1;
}

int handle_raw_hid_device_missing(raw_hid_node_t* previous_node, raw_hid_node_t* current_node) {
    // returns 1 if the node was freed, 0 if the node was marked for unregistration, -1 for error
    if (atomic_load(&(current_node->is_marked_for_deletion))) {
        if (previous_node != NULL) {
            atomic_store(&(previous_node->next), atomic_load(&(current_node->next)));
        } else {
            atomic_store(&raw_hid_nodes, atomic_load(&(current_node->next)));
        }
        atomic_store(&main_loop_new_iteration_flag, false);
        // wait until we're certain the main process isn't on this node
        while (!atomic_load(&main_loop_new_iteration_flag)) {
#ifdef _WIN32
            Sleep(SLEEP_MILLISECONDS_WINDOWS);
#else
            sleep_milliseconds(SLEEP_MILLISECONDS_POSIX);
#endif
        }
        raw_hid_node_free(current_node);
        return 1;
    } else {
        atomic_store(&(current_node->is_marked_for_unregistration), true);
        return 0;
    }
}

void enumerate_raw_hid_devices(void) {

    // unmark existing open devices
    raw_hid_node_t* current_node = atomic_load(&raw_hid_nodes);
    while (current_node != NULL) {
        current_node->is_in_enumeration = false;
        current_node = atomic_load(&(current_node->next));
    }
    
    // open any newly found devices
    struct hid_device_info* hid_enumeration_start = hid_enumerate(0x0, 0x0);
    struct hid_device_info* current_device_info = hid_enumeration_start;
    int result = 0;
    while (current_device_info != NULL) {
        if (current_device_info->usage_page == QMK_RAW_HID_USAGE_PAGE && current_device_info->usage == QMK_RAW_HID_USAGE) {
            result = handle_raw_hid_device_found(current_device_info->path);
            if (verbose_basic && result == 1) {
                printf("Opened a new raw HID device:\n");
                print_device_info(current_device_info);
            }
        }
        current_device_info = current_device_info->next;
    }
    hid_free_enumeration(hid_enumeration_start);

    // close devices that weren't found in the enumeration
    current_node = atomic_load(&raw_hid_nodes);
    raw_hid_node_t* previous_node = NULL;
    while (current_node != NULL) {
        if (!current_node->is_in_enumeration) {
            result = handle_raw_hid_device_missing(previous_node, current_node);
            if (verbose_basic && result == 1) {
                printf("Closed a missing raw HID device.\n");
            }
        }
        previous_node = current_node;
        current_node = atomic_load(&(current_node->next));
    }
}

// ============================================================================
// raw_hid_message_t MEMORY MANAGEMENT (parent only)
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
// DEVICE REGISTRATION/UNREGISTRATION (parent only)
// ============================================================================

int register_node(raw_hid_node_t* node) {
    // returns 1 if the registration was successful, 0 if the node was already registered, -1 for error
    if (DEVICE_ID_IS_VALID(node->device_id)) {
        return 0;
    }
    if (n_registered_devices == MAX_REGISTERED_DEVICES) {
        if (verbose_basic) {
            printf("Too many registered devices.\n");
        }
        return -1;
    }
    node->device_id = next_unassigned_device_id;
    device_id_is_assigned[next_unassigned_device_id] = true;
    while (device_id_is_assigned[next_unassigned_device_id]) {
        next_unassigned_device_id = (next_unassigned_device_id + 1) % N_UNIQUE_DEVICE_IDS;
    }
    assigned_device_ids[n_registered_devices] = node->device_id;
    n_registered_devices += 1;
    if (verbose_basic) {
        printf("Device was registered with ID: 0x%02hx\n", node->device_id);
    }
    registrations_changed = true;
    return 1;
}

void unregister_node(raw_hid_node_t* node) {
    if (node->device_id == DEVICE_ID_UNASSIGNED) {
        return;
    }
    if (verbose_basic) {
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
    registrations_changed = true;
}

// ============================================================================
// ACTUAL COMMUNICATION (parent only)
// ============================================================================

void communicate_with_raw_hid_device(raw_hid_node_t* node) {

    // read from device
    int bytes_read = hid_read(node->device, buffer_data, QMK_RAW_HID_REPORT_SIZE);
    int result;
    unsigned char destination_device_id;
    while (bytes_read > 0) {
        if (buffer_data[0] != RAW_HID_HUB_COMMAND_ID) {
            if (verbose_discard) {
                printf("Discarding:          ");
                print_buffer();
            }
            goto next_hid_read;
        } else {
            if ((verbose_hub && buffer_data[1] == DEVICE_ID_HUB)) {
                printf("Receiving from 0x%02hx: ", node->device_id);
                print_buffer();
            }

            // registration report
            if (buffer_data[1] == DEVICE_ID_HUB && buffer_data[2] == 0x01) {
                if (verbose_stats) {
                    message_counter_increment(node->device_id, DEVICE_ID_HUB);
                }
                result = register_node(node);
                if (result == 0) {
                    // registrations didn't change, so respond to only this device
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
                    if (verbose_stats) {
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
                if (verbose_stats) {
                    message_counter_increment(node->device_id, DEVICE_ID_HUB);
                }
                unregister_node(node);
                goto next_hid_read;
            }

            // message report
            if (buffer_data[1] != DEVICE_ID_HUB) {
                destination_device_id = buffer_data[1]; 
                if (!device_id_is_assigned[destination_device_id]) {
                    goto next_hid_read;
                }
                buffer_data[1] = node->device_id;
                message_queue_push(destination_device_id, buffer_data);
                if (verbose_stats) {
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

    // queue up status reports
    if (registrations_changed) {
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
            if (verbose_stats) {
                message_counter_increment(DEVICE_ID_HUB, destination_device_id);
            }
        }
        registrations_changed = false;
    }

    // send to device
    while (device_id_message_queue[node->device_id] != NULL) {
        message_queue_pop(node->device_id, buffer_data);
        if ((verbose_hub && buffer_data[1] == DEVICE_ID_HUB) || (verbose_device && buffer_data[1] != DEVICE_ID_HUB)) {
            printf("Sending to 0x%02hx:     ", node->device_id);
            print_buffer();
        }
        hid_write(node->device, buffer_report_id_and_data, QMK_RAW_HID_REPORT_SIZE + 1);
    }
}

void iterate_over_raw_hid_devices(void) {
    raw_hid_node_t* current_node = atomic_load(&raw_hid_nodes);
    while (current_node != NULL) {
        if (atomic_load((&(current_node->is_marked_for_unregistration)))) {
            unregister_node(current_node);
            atomic_store((&(current_node->is_marked_for_deletion)), true);
        } else {
            communicate_with_raw_hid_device(current_node);
        }
        current_node = atomic_load(&(current_node->next));
    }
    atomic_store(&main_loop_new_iteration_flag, true);
}

void send_hub_shutdown_reports(void) {
    buffer_data[0] = RAW_HID_HUB_COMMAND_ID;
    buffer_data[1] = DEVICE_ID_HUB;
    buffer_data[2] = DEVICE_ID_UNASSIGNED;
    raw_hid_node_t* current_node = atomic_load(&raw_hid_nodes);
    while (current_node != NULL) {
        if (DEVICE_ID_IS_VALID(current_node->device_id)) {
            hid_write(current_node->device, buffer_report_id_and_data, QMK_RAW_HID_REPORT_SIZE + 1);
        }
        current_node = atomic_load(&(current_node->next));
    }
}

// ============================================================================
// CHILD PROCESS FOR ENUMERATION
// ============================================================================

#ifdef _WIN32
HANDLE hChildProcess = NULL;
#else
pthread_t child_thread;
#endif

#ifdef _WIN32
void child_process(void) {
    while (!atomic_load(&child_termination_flag)) {
        enumerate_raw_hid_devices();
        Sleep(SECONDS_PER_ENUMERATION * 1000);
    }
}
#else
void* child_process(void* arg) {
    while (!atomic_load(&child_termination_flag)) {
        enumerate_raw_hid_devices();
        sleep_milliseconds(SECONDS_PER_ENUMERATION * 1000);
    }
}
#endif

void start_child(void) {
#ifdef _WIN32
    hChildProcess = (HANDLE)_beginthread((void (*)(void *))child_process, 0, NULL);
    if (hChildProcess == NULL) {
        printf("Error creating parallel process/thread for enumeration.");
        exit(1);
    }
#else
    int result = pthread_create(&child_thread, NULL, (void* (*)(void*))child_process, NULL);
    if (result != 0) {
        printf("Error creating parallel process/thread for enumeration.");
        exit(1);
    }
#endif
}

void stop_child(void) {
    atomic_store(&main_loop_new_iteration_flag, true);
    atomic_store(&child_termination_flag, true);
#ifdef _WIN32
    if (hChildProcess != NULL) {
        WaitForSingleObject(hChildProcess, INFINITE);
        CloseHandle(hChildProcess);
        hChildProcess = NULL;
    }
#else
    pthread_join(child_thread, NULL);
#endif
}

// ============================================================================
// CLEANUP ON EXIT
// ============================================================================

void cleanup(void) {
    send_hub_shutdown_reports();
    stop_child();
    raw_hid_node_free_all();
    message_queue_clear_all();
    message_counter_free_all();
    hid_exit();
    if (verbose_basic) {
        printf("Cleanup completed.\n");
    }
}

void signal_handler(int signal) {
    cleanup();
    exit(signal);
}

// ============================================================================
// MAIN
// ============================================================================

void parse_verbose(int argc, char* argv[]) {
    // crude parser for verbose argument
    uint8_t verbose = 0;
    if (argc > 1 && strncmp(argv[1], "-v", 2) == 0) {
        verbose = atoi(&argv[1][2]);
    }
    if (verbose > 0) {
        printf("Verbose:\n");
        if (verbose % 2 == 1) {
            verbose_basic = true;
            printf("  Printing basic status messages.\n");
        }
        verbose /= 2;
        if (verbose % 2 == 1) {
            verbose_stats = true;
            printf("  Printing stats.\n");
        }
        verbose /= 2;
        if (verbose % 2 == 1) {
            verbose_hub = true;
            printf("  Printing messages to and from the hub.\n");
        }
        verbose /= 2;
        if (verbose % 2 == 1) {
            verbose_device = true;
            printf("  Printing messages between registered devices.\n");
        }
        verbose /= 2;
        if (verbose % 2 == 1) {
            verbose_discard = true;
            printf("  Printing discarded reports.\n");
        }
    }
}

void main_sleep(void) {
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
        if (last_message_time_ms - current_time_ms > SMART_SLEEP_WAIT_MILLISECONDS_POSIX) {
            sleep_milliseconds(SLEEP_MILLISECONDS_POSIX);
        }
#    else
        sleep_milliseconds(SLEEP_MILLISECONDS_POSIX);
#    endif
#endif
}

int main(int argc, char* argv[])
{
    parse_verbose(argc, argv);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGABRT, signal_handler);

    // initialize hidapi
    int result = hid_init();
    if (result == -1)
        // print statement causes initialization to fail?!
        // printf("HIDAPI initialization unsuccessful.\n");
        return -1;
#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
    hid_darwin_set_open_exclusive(0);
#endif
    if (verbose_basic) {
        printf("HIDAPI initialization successful.\n");
    }

    // initialize global variables
    memset(device_id_is_assigned, false, sizeof(device_id_is_assigned));
    memset(assigned_device_ids, DEVICE_ID_UNASSIGNED, sizeof(assigned_device_ids));
    memset(buffer_report_id_and_data, 0, sizeof(buffer_report_id_and_data));
    memset(device_id_message_queue, 0, sizeof(device_id_message_queue));
    buffer_report_id_and_data[0] = QMK_RAW_HID_REPORT_ID;
    buffer_data = buffer_report_id_and_data + 1;
    update_current_time_ms();
    last_stats_time_ms = current_time_ms;
    last_message_time_ms = current_time_ms;

    // start a child thread to run periodic enumerations
    start_child();

    // main loop
    while (true) {

        // update time
        update_current_time_ms();

        // actual hid task
        iterate_over_raw_hid_devices();

        // print stats
        maybe_print_and_update_stats();

        // sleep to reduce resource usage
        main_sleep();
    }
    
    // cleanup
    cleanup();
    return 0;
}
