/**
 * iRecovery - Utility for DFU 2.0, WTF and Recovery Mode
 * Copyright (C) 2008 - 2009 westbaer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#include "libirecovery.h"

#define BUFFER_SIZE 0x1000
#define debug(...) if(client->debug) fprintf(stderr, __VA_ARGS__)

int irecv_default_sender(irecv_client_t client, unsigned char* data, int size);
int irecv_default_receiver(irecv_client_t client, unsigned char* data, int size);

irecv_error_t irecv_open(irecv_client_t* pclient) {
	int i = 0;
	char serial[256];
	struct libusb_device* usb_device = NULL;
	struct libusb_context* usb_context = NULL;
	struct libusb_device** usb_device_list = NULL;
	struct libusb_device_handle* usb_handle = NULL;
	struct libusb_device_descriptor usb_descriptor;

	*pclient = NULL;
	libusb_init(&usb_context);
	irecv_error_t error = IRECV_E_SUCCESS;
	int usb_device_count = libusb_get_device_list(usb_context, &usb_device_list);
	for (i = 0; i < usb_device_count; i++) {
		usb_device = usb_device_list[i];
		libusb_get_device_descriptor(usb_device, &usb_descriptor);
		if (usb_descriptor.idVendor == APPLE_VENDOR_ID) {
			/* verify this device is in a mode we understand */
			if (usb_descriptor.idProduct == kRecoveryMode1 ||
				usb_descriptor.idProduct == kRecoveryMode2 ||
				usb_descriptor.idProduct == kRecoveryMode3 ||
				usb_descriptor.idProduct == kRecoveryMode4 ||
				usb_descriptor.idProduct == kDfuMode) {

				libusb_open(usb_device, &usb_handle);
				if (usb_handle == NULL) {
					libusb_free_device_list(usb_device_list, 1);
					libusb_close(usb_handle);
					libusb_exit(usb_context);
					return IRECV_E_UNABLE_TO_CONNECT;
				}
				libusb_set_debug(usb_context, 3);

				libusb_free_device_list(usb_device_list, 0);

				irecv_client_t client = (irecv_client_t) malloc(sizeof(struct irecv_client));
				if (client == NULL) {
					libusb_close(usb_handle);
					libusb_exit(usb_context);
					return IRECV_E_OUT_OF_MEMORY;
				}
				memset(client, '\0', sizeof(struct irecv_client));
				client->interface = -1;
				client->handle = usb_handle;
				client->context = usb_context;
				client->mode = usb_descriptor.idProduct;

				error = irecv_set_configuration(client, 1);
				if(error != IRECV_E_SUCCESS) {
					return error;
				}

				*pclient = client;
				return IRECV_E_SUCCESS;
			}
		}
	}

	return IRECV_E_UNABLE_TO_CONNECT;
}

irecv_error_t irecv_set_configuration(irecv_client_t client, int configuration) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	debug("Setting to configuration %d", configuration);
	
	int current = 0;
	libusb_get_configuration(client->handle, &current);
	if(current != configuration) {
		if (libusb_set_configuration(client->handle, configuration) < 0) {
			return IRECV_E_USB_CONFIGURATION;
		}
	}
	
	client->config = configuration;
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_set_interface(irecv_client_t client, int interface, int alt_interface) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	if(client->interface == interface) {
		return IRECV_E_SUCCESS;
	}

	debug("Setting to interface %d:%d", interface, alt_interface);
	if (libusb_claim_interface(client->handle, interface) < 0) {
		return IRECV_E_USB_INTERFACE;
	}
	
	if(libusb_set_interface_alt_setting(client->handle, interface, alt_interface) < 0) {
		return IRECV_E_USB_INTERFACE;
	}
	
	client->interface = interface;
	client->alt_interface = alt_interface;
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_reset(irecv_client_t client) {
	if (client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	libusb_reset_device(client->handle);
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_close(irecv_client_t client) {
	if (client != NULL) {
		if (client->handle != NULL) {
			if(client->interface >= 0) {
				libusb_release_interface(client->handle, client->interface);
			}
			libusb_close(client->handle);
			client->handle = NULL;
		}

		if (client->context != NULL) {
			libusb_exit(NULL);
			client->context = NULL;
		}

		free(client);
		client = NULL;
	}

	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_set_debug(irecv_client_t client, int level) {
	if(client == NULL || client->context == NULL) {
		return IRECV_E_NO_DEVICE;
	}
	
	libusb_set_debug(client->context, level);
	client->debug = level;
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send(irecv_client_t client, unsigned char* command) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	unsigned int length = strlen(command);
	if(length >= 0x100) {
		length = 0xFF;
	}
	
	if(client->send_callback != NULL) {
		// Call our user defined callback first, this must return a number of bytes to send
		//   or zero to abort send.
		length = client->send_callback(client, command, length);
	}

	if(length > 0) {
		irecv_send_command(client, command);
	}

	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_command(irecv_client_t client, unsigned char* command) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	irecv_error_t error = irecv_set_interface(client, 1, 1);
	if(error != IRECV_E_SUCCESS) {
		return error;
	}

	unsigned int length = strlen(command);
	if(length >= 0x100) {
		length = 0xFF;
	}

	if(length > 0) {
		libusb_control_transfer(client->handle, 0x40, 0, 0, 0, command, length+1, 100);
	}
	
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_file(irecv_client_t client, const char* filename) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}
	
	FILE* file = fopen(filename, "rb");
	if (file == NULL) {
		return IRECV_E_FILE_NOT_FOUND;
	}

	fseek(file, 0, SEEK_END);
	int length = ftell(file);
	fseek(file, 0, SEEK_SET);

	unsigned char* buffer = (unsigned char*) malloc(length);
	if (buffer == NULL) {
		fclose(file);
		return IRECV_E_OUT_OF_MEMORY;
	}

	int bytes = fread(buffer, 1, length, file);
	fclose(file);

	if(bytes != length) {
		free(buffer);
		return IRECV_E_UNKNOWN_ERROR;
	}

	irecv_error_t error = irecv_send_buffer(client, buffer, length);
	free(buffer);
	return error;
}

irecv_error_t irecv_get_status(irecv_client_t client, unsigned int* status) {
	if(client == NULL || client->handle == NULL) {
		*status = 0;
		return IRECV_E_NO_DEVICE;
	}
	
	irecv_error_t error = irecv_set_interface(client, 1, 1);
	if(error != IRECV_E_SUCCESS) {
		return error;
	}

	unsigned char buffer[6];
	memset(buffer, '\0', 6);
	if(libusb_control_transfer(client->handle, 0xA1, 3, 0, 0, buffer, 6, 1000) != 6) {
		*status = 0;
		return IRECV_E_USB_STATUS;
	}
	
	debug("status: %d\n", (unsigned int) buffer[4]);
	*status = (unsigned int) buffer[4];
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_buffer(irecv_client_t client, unsigned char* buffer, unsigned int length) {
	irecv_error_t error = 0;
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	error = irecv_set_interface(client, 1, 1);
	if(error != IRECV_E_SUCCESS) {
		return error;
	}

	int last = length % 0x800;
	int packets = length / 0x800;
	if (last != 0) {
		packets++;
	}

	int i = 0;
	unsigned int status = 0;
	for (i = 0; i < packets; i++) {
		int size = i + 1 < packets ? 0x800 : last;
		int bytes = libusb_control_transfer(client->handle, 0x21, 1, 0, 0, &buffer[i * 0x800], size, 1000);
		if (bytes != size) {
			return IRECV_E_USB_UPLOAD;
		}
		
		debug("Sent %d bytes\n", bytes);

		error = irecv_get_status(client, &status);
		if (error != IRECV_E_SUCCESS) {
			return error;
		}

		if(status != 5) {
			return IRECV_E_USB_UPLOAD;
		}

	}

	libusb_control_transfer(client->handle, 0x21, 1, 0, 0, buffer, 0, 1000);
	for (i = 0; i < 3; i++) {
		error = irecv_get_status(client, &status);
		if(error != IRECV_E_SUCCESS) {
			return error;
		}
	}

	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_receive(irecv_client_t client) {
	unsigned char buffer[BUFFER_SIZE];
	memset(buffer, '\0', BUFFER_SIZE);
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	irecv_error_t error = irecv_set_interface(client, 1, 1);
	if(error != IRECV_E_SUCCESS) {
		return error;
	}

	int bytes = 0;
	while(libusb_bulk_transfer(client->handle, 0x81, buffer, BUFFER_SIZE, &bytes, 100) == 0) {
		if(bytes > 0) {
			if(client->receive_callback != NULL) {
				if(client->receive_callback(client, buffer, bytes) != bytes) {
					return IRECV_E_UNKNOWN_ERROR;
				}
			}
		} else break;
	}
	
	return IRECV_E_SUCCESS;
}

int irecv_default_sender(irecv_client_t client, unsigned char* data, int size) {
	return size;
}

int irecv_default_receiver(irecv_client_t client, unsigned char* data, int size) {
	int i = 0;
	for(i = 0; i < size; i++) {
		printf("%c", data[i]);
	}
	return size;
}

irecv_error_t irecv_set_receiver(irecv_client_t client, irecv_receive_callback callback) {
	if(client == NULL) {
		return IRECV_E_NO_DEVICE;
	}
	
	client->receive_callback = callback;
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_set_sender(irecv_client_t client, irecv_send_callback callback) {
	if(client == NULL) {
		return IRECV_E_NO_DEVICE;
	}
	
	client->send_callback = callback;
	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_getenv(irecv_client_t client, unsigned char** var) {
	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	irecv_error_t error = irecv_set_interface(client, 1, 1);
	if(error != IRECV_E_SUCCESS) {
		return error;
	}

	unsigned char* value = (unsigned char*) malloc(256);
	if(value == NULL) {
		return IRECV_E_OUT_OF_MEMORY;
	}

	int ret =  libusb_control_transfer(client->handle, 0xC0, 0, 0, 0, value, 256, 500);
	if(ret < 0) {
		return IRECV_E_UNKNOWN_ERROR;
	}

	*var = value;
	return IRECV_E_SUCCESS;
}


irecv_error_t irecv_get_ecid(irecv_client_t client, unsigned long long* ecid) {
	char info[256];
	memset(info, '\0', 256);

	if(client == NULL || client->handle == NULL) {
		return IRECV_E_NO_DEVICE;
	}

	libusb_get_string_descriptor_ascii(client->handle, 3, info, 255);
	printf("%d: %s\n", strlen(info), info);

	unsigned char* ecid_string = strstr(info, "ECID:");
	if(ecid_string == NULL) {
		*ecid = 0;
		return IRECV_E_UNKNOWN_ERROR;
	}
	sscanf(ecid_string, "ECID:%qX", ecid);

	irecv_reset(client);
	return IRECV_E_SUCCESS;
}

const char* irecv_strerror(irecv_error_t error) {
	switch(error) {
	case IRECV_E_SUCCESS:
		return "Command completed successfully";
		
	case IRECV_E_NO_DEVICE:
		return "Unable to find device";
		
	case IRECV_E_OUT_OF_MEMORY:
		return "Out of memory";
		
	case IRECV_E_UNABLE_TO_CONNECT:
		return "Unable to connect to device";
		
	case IRECV_E_INVALID_INPUT:
		return "Invalid input";
		
	case IRECV_E_FILE_NOT_FOUND:
		return "File not found";
		
	case IRECV_E_USB_UPLOAD:
		return "Unable to upload data to device";
		
	case IRECV_E_USB_STATUS:
		return "Unable to get device status";
		
	case IRECV_E_USB_INTERFACE:
		return "Unable to set device interface";
		
	case IRECV_E_USB_CONFIGURATION:
		return "Unable to set device configuration";
		
	default:
		return "Unknown error";
	}
	
	return NULL;
}
