/*
 * Copyright (C) 2015-2020 "IoT.bzh"
 * Author José Bollo <jose.bollo@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <stdbool.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-bus-protocol.h>
#include <json-c/json.h>

#define AFB_BINDING_VERSION 4
#include <afb/afb-binding.h>
#include <pcsc-glue.h>
#include "dbus-jsonc.h"

/**
* busnames
*/
#define BUSNAME_USER   "user"
#define BUSNAME_SYSTEM "system"
#if !defined(DEFAULT_BUSNAME)
#define DEFAULT_BUSNAME BUSNAME_SYSTEM
#endif

/**
* name of the default event
*/
#define DEFAULT_EVENT_NAME "default"

/**
* size of the job queue
*/
#define MXNRJOB 10

// nfc event
static afb_event_t event_nfc;

/**
* structure for event/signal specification
*/
struct evsigspec
{
	const char *busname;
	const char *match;
	const char *event;
};

/**
* structure for named events
*/
struct evrec
{
	/** link to next */
	struct evrec *next;
	/** the event */
	afb_event_t event;
	/** reference count */
	unsigned refcnt;
	/** name */
	char name[];
};

/**
* structure for referencing events
*/
struct evlist
{
	/** link to next */
	struct evlist *next;
	/** link to the event */
	struct evrec *evrec;
	/** reference count */
	unsigned refcnt;
};

/**
* structure for watchers of signals
*/
struct watch
{
	/** link to next */
	struct watch *next;
	/** attached events */
	struct evlist *evlist;
	/** slot for removal */
	sd_bus_slot *slot;
	/** name of bus */
	const char *busname;
	/** match filter */
	const char *match;
};

/** global mutex */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/** SD event loop */
static sd_event *sdevlp = NULL;

/** event loop wake up channel */
static int efd = 0;

/** pending request jobs */
static afb_req_t reqs[MXNRJOB];
static void (*procs[MXNRJOB])(afb_req_t);
static int njob = 0;

/** the list of activated watches */
static struct watch *watchers = NULL;

/** the list of named events */
static struct evrec *evts = NULL;

/*****************************************************************************************/
/* helpers */
/*****************************************************************************************/

/*
 * Get the string of 'key' from 'obj'
 * Returns defval if 'key' isn't in 'obj' or isn't a string
 */
static const char *strval(struct json_object *obj, const char *key, const char *defval)
{
	struct json_object *keyval;
	return json_object_object_get_ex(obj, key, &keyval) && json_object_is_type(keyval, json_type_string)
	    		? json_object_get_string(keyval)
			: defval;
}

/* creates the error object for the dbus error */
static struct json_object *jsonc_of_dbus_error(const sd_bus_error *err)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "DBus-error-name", json_object_new_string(err->name));
	json_object_object_add(obj, "DBus-error-message", json_object_new_string(err->message));
	return obj;
}

/* generic unlink of an item of a list pnxt (with next on first position) */
static void unlinklistitem(void *item, void *pnxt)
{
	void *nxt = *(void**)pnxt;
	if (nxt != item)
		unlinklistitem(item, nxt);
	else
		*(void**)pnxt = *(void**)item;
}

/* unlink and free the item from the list pnxt */
static void removelistitem(void *item, void *pnxt)
{
	unlinklistitem(item, pnxt);
	free(item);
}

/*****************************************************************************************/
/* bus provider */
/*****************************************************************************************/

/* returns the standard bus name or NULL is busname is illegal */
static const char *std_busname(const char *busname)
{
	return busname == NULL ? DEFAULT_BUSNAME
		: !strcmp(busname, BUSNAME_SYSTEM) ? BUSNAME_SYSTEM
		: !strcmp(busname, BUSNAME_USER) ? BUSNAME_USER
		: NULL;
}

/* returns the DBUS to use */
static struct sd_bus *getbus(const char *busname)
{
	static struct sd_bus *buses[2];
	static const char *names[2] = { BUSNAME_USER, BUSNAME_SYSTEM };
	static int (*creators[2])(struct sd_bus**);

	struct sd_bus *result = NULL;
	int rc, index = 2;
	for (;;) {
		/* check if end */
		if (!index)
			break;
		/* check if found */
		if (strcmp(busname, names[--index]))
			continue;
		/* check if available */
		result = buses[index];
		if (result != NULL)
			break;
		/* create */
		rc = (index ? sd_bus_default_system : sd_bus_default_user)(&result);
		if (rc >= 0) {
			/* attach to the main loop */
			rc = sd_bus_attach_event(result, sdevlp, SD_EVENT_PRIORITY_NORMAL);
			if (rc >= 0) {
				/* record result */
				buses[index] = result;
				break;
			}
			sd_bus_unref(result);
		}
		/* error found */
		AFB_ERROR("creation of SDBUS %s failed", names[index]);
		result = NULL;
		break;
	}
	return result;
}

/*****************************************************************************************/
/* DBUS thread and and its job control */
/*****************************************************************************************/

/* submit a request that will be processed by  the given proc in the DBUS thread context */
static void submit(afb_req_t req, void (*proc)(afb_req_t))
{
	pthread_mutex_lock(&mutex);
	if (sdevlp == NULL) {
		pthread_mutex_unlock(&mutex);
		afb_req_reply(req, AFB_ERRNO_INTERNAL_ERROR, 0, NULL);
		AFB_ERROR("No event loop");
	}
	else if (njob == MXNRJOB) {
		/* ooooch! too many jobs !!! */
		pthread_mutex_unlock(&mutex);
		afb_req_reply(req, AFB_ERRNO_INTERNAL_ERROR, 0, NULL);
		AFB_ERROR("Too many requests");
	}
	else {
		/* first, shift the pending job queue */
		uint64_t inc = 1;
		int jdx, idx = njob++;
		while (idx) {
			jdx = idx - 1;
			reqs[idx] = reqs[jdx];
			procs[idx] = procs[jdx];
			idx = jdx;
		}
		/* add the given job */
		reqs[0] = afb_req_addref(req);
		procs[0] = proc;
		pthread_mutex_unlock(&mutex);
		/* signal the DBUS thread that a new job is queued */
		write(efd, & inc, sizeof inc);
	}
}

/* DBUS thread simply runs the sd_event loop forever */
static int gotjob(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	uint64_t count;
	afb_req_t req;
	void (*proc)(afb_req_t);

	read(efd, &count, sizeof count);
	for (;;) {
		pthread_mutex_lock(&mutex);
		if (njob == 0) {
			pthread_mutex_unlock(&mutex);
			return 0;
		}
		req = reqs[--njob];
		proc = procs[njob];
		pthread_mutex_unlock(&mutex);
		proc(req);
		afb_req_unref(req);
	}
}

/* DBUS thread simply runs the sd_event loop forever */
static void *run(void *argh)
{
	pthread_mutex_lock(&mutex);
	/* create the event loop */
	int rc = sd_event_default(&sdevlp);
	if (rc >= 0) {
		/* attach the loop signaler */
		rc = sd_event_add_io(sdevlp, NULL, efd, EPOLLIN, gotjob, NULL);
		if (rc >= 0) {
			pthread_mutex_unlock(&mutex);
			sd_event_loop(sdevlp);
			pthread_mutex_lock(&mutex);
		}
		sd_event_unref(sdevlp);
		sdevlp = NULL;
	}
	pthread_mutex_unlock(&mutex);
	return NULL;
}

/*****************************************************************************************/
/* manage afb event records (evrec) */
/*****************************************************************************************/

/* search in the list */
static struct evrec *search_evrec(const char *name)
{
	struct evrec *evrec = evts;
	while(evrec != NULL && strcmp(name, evrec->name))
		evrec = evrec->next;
	return evrec;
}

/* create and add in the list */
static struct evrec *create_evrec(afb_api_t api, const char *name)
{
	struct evrec *evrec = malloc(sizeof *evrec + 1 + strlen(name));
	if (evrec != NULL) {
		int rc = afb_api_new_event(api, name, &evrec->event);
		if (rc < 0) {
			free(evrec);
			evrec = NULL;
		}
		else {
			strcpy(evrec->name, name);
			evrec->refcnt = 0;
			evrec->next = evts;
			evts = evrec;
		}
	}
	return evrec;
}

/* remove from the list */
static void remove_evrec(struct evrec *evrec)
{
	removelistitem(evrec, &evts);
}

/*****************************************************************************************/
/* manage dbus matchers (watch) */
/*****************************************************************************************/

/* search in the list */
static struct watch *search_watch(struct evsigspec *evs)
{
	struct watch *watch;
	for (watch = watchers ; watch != NULL ; watch = watch->next) {
		if (!strcmp(evs->busname, watch->busname) && !strcmp(evs->match, watch->match))
			break;
	}
	return watch;
}

/* create and add in the list */
static struct watch *create_watch(struct evsigspec *evs)
{
	struct watch *watch;
	watch = malloc(sizeof *watch + strlen(evs->busname) + strlen(evs->match));
	if (watch != NULL) {
		char *p = (char*)&watch[1];
		watch->busname = p;
		p = 1 + stpcpy(p, evs->busname);
		watch->match = p;
		p = 1 + stpcpy(p, evs->match);
		watch->evlist = NULL;
		watch->slot = NULL;
		watch->next = watchers;
		watchers = watch;
	}
	return watch;
}

/* remove from the list */
static void remove_watch(struct watch *watch)
{
	removelistitem(watch, &watchers);
}

/*****************************************************************************************/
/* manage items linking matches (watch) to events */
/*****************************************************************************************/

/* search in the list */
static struct evlist *search_evlist(struct watch *watch, struct evrec *evrec)
{
	struct evlist *evlist = watch->evlist;
	while(evlist != NULL && evlist->evrec != evrec)
		evlist = evlist->next;
	return evlist;
}

/* create and add in the list */
static struct evlist *create_evlist(struct watch *watch, struct evrec *evrec)
{
	struct evlist *evlist = malloc(sizeof *evlist);
	if (evlist != NULL) {
		evlist->evrec = evrec;
		evlist->refcnt = 0;
		evlist->next = watch->evlist;
		watch->evlist = evlist;
	}
	return evlist;
}

/* remove from the list */
static void remove_evlist(struct watch *watch, struct evlist *evlist)
{
	removelistitem(evlist, &watch->evlist);
}

/*****************************************************************************************/
/* manage subscriptions */
/*****************************************************************************************/

/* propagate the received DBUS signal to afb listeners */
static int on_signal(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
	struct watch *watch = userdata;
	struct evlist *evlist;
	struct json_object *obj, *data = NULL;
	afb_data_t adat;
	int rc = -1;
	const sd_bus_error *err;

	/* check if error */
	err = sd_bus_message_get_error(msg);
	if (err != NULL)
		data = jsonc_of_dbus_error(err);
	else
		rc = msg2jsonc(msg, &data);

	/* make the sent event */
	obj = json_object_new_object();
	json_object_object_add(obj, "bus", json_object_new_string(watch->busname));
	json_object_object_add(obj, "status", json_object_new_string(rc >= 0 ? "success" : "error"));
	json_object_object_add(obj, "data", data);
	json_object_object_add(obj, "sender",    json_object_new_string(sd_bus_message_get_sender(msg)));
	json_object_object_add(obj, "path",      json_object_new_string(sd_bus_message_get_path(msg)));
	json_object_object_add(obj, "interface", json_object_new_string(sd_bus_message_get_interface(msg)));
	json_object_object_add(obj, "member",    json_object_new_string(sd_bus_message_get_member(msg)));

	/* send the event now */
	afb_create_data_raw(&adat, AFB_PREDEFINED_TYPE_JSON_C, obj, 0, (void*)json_object_put, obj);
	evlist = watch->evlist;
	while (evlist != NULL) {
		afb_data_addref(adat);
		afb_event_push(evlist->evrec->event, 1, &adat);
		evlist = evlist->next;
	}
	afb_data_unref(adat);
	return 1;
}

/* process subscribe and unsubscribe requests */
static void process_sub(afb_req_t req, int dir)
{
	afb_data_t first_arg;
	struct json_object *obj;

	struct evsigspec evs;
	struct watch *watch;
	struct evrec *evrec;
	struct evlist *evlist;

	struct sd_bus *bus;
	int rc;
	int issys, add;

	/* get the query */
	rc = afb_req_param_convert(req, 0, AFB_PREDEFINED_TYPE_JSON_C, &first_arg);
	if (rc < 0)
		goto bad_request;
	obj = (struct json_object*)afb_data_ro_pointer(first_arg);
	if (obj == NULL)
		goto bad_request;

	/* get parameters */
	evs.busname     = strval(obj, "bus",       NULL);
	evs.match       = strval(obj, "match",     NULL);
	evs.event       = strval(obj, "event",     DEFAULT_EVENT_NAME);

	/* check parameters */
	evs.busname = std_busname(evs.busname);
	if (evs.busname == NULL || evs.match == NULL)
		goto bad_request;
	bus = getbus(evs.busname);
	if (bus == NULL)
		goto internal_error;

	/* search the watcher and the event */
	watch = search_watch(&evs);
	evrec = search_evrec(evs.event);

	if (dir > 0) {
		/* subscribing */
		if (evrec == NULL)
			evrec = create_evrec(afb_req_get_api(req), evs.event);
		if (watch == NULL)
			watch = create_watch(&evs);
		if (watch == NULL || evrec == NULL)
			goto internal_error;

		evlist = search_evlist(watch, evrec);
		if (evlist != NULL)
			evlist->refcnt++;
		else {
			/* add the link */
			evlist = create_evlist(watch, evrec);
			if (evlist == NULL)
				goto internal_error;
			evlist->refcnt++;
			evrec->refcnt++;

			/* process DBUS subscription */
			rc = sd_bus_add_match_async(bus, &watch->slot, watch->match,
					on_signal, NULL, watch);
			if (rc < 0) {
				process_sub(req, 0);
				return;
			}
		}
		afb_req_subscribe(req, evrec->event);
	}
	else {
		/* unsubscribing */
		evlist = watch != NULL && evrec != NULL ? search_evlist(watch, evrec) : NULL;
		if (evlist == NULL)
			goto bad_request;

		afb_req_unsubscribe(req, evrec->event);
		if (evlist->refcnt > 1)
			evlist->refcnt--;
		else {
			remove_evlist(watch, evlist);
			if (watch->evlist == NULL) {
				sd_bus_slot_unref(watch->slot);
				remove_watch(watch);
			}
			if (evrec->refcnt > 1)
				evrec->refcnt--;
			else {
				afb_event_unref(evrec->event);
				remove_evrec(evrec);
			}
		}
	}
	afb_req_reply(req, 0, 0, NULL);
	return;

bad_request:
	if (dir) {
		afb_req_reply(req, AFB_ERRNO_INVALID_REQUEST, 0, NULL);
		return;
	}
internal_error:
	afb_req_reply(req, AFB_ERRNO_INTERNAL_ERROR, 0, NULL);
}

static void process_subscribe(afb_req_t req)
{
	process_sub(req, 1);
}

static void process_unsubscribe(afb_req_t req)
{
	process_sub(req, -1);
}

static void check_nfc_cb(afb_timer_t timer, void *closure, unsigned decount)
{
	bool nfc_state = false;
	afb_data_t ev_data;
	// from the pcsd library
	pcscHandleT *handle;
	ulong readerCount = 2;
    const char *readerList[readerCount];

	// AFB_NOTICE("Check nfc...");

	handle=pcscList(readerList, &readerCount);

	if(!handle)
		AFB_ERROR("Failed to connect to pcscd daemon");
	else
	{
		nfc_state=true;

		if(readerCount)
		{
			afb_create_data_copy(&ev_data, AFB_PREDEFINED_TYPE_STRINGZ, readerList[0], strlen(readerList[0]) + 1);
			afb_event_push(event_nfc, 1, &ev_data);
			afb_timer_unref(timer);
		}
		
		// AFB_NOTICE("%lu , %s",readerCount,readerList[0]);
	}

}

/*****************************************************************************************/
/* manage signals */
/*****************************************************************************************/

/* process signal requests */
static void process_signal(afb_req_t req)
{
	afb_data_t first_arg;
	struct json_object *obj;
	struct json_object *args;

	const char *busname;
	const char *destination;
	const char *path;
	const char *interface;
	const char *member;
	const char *signature;

	struct sd_bus_message *msg = NULL;
	struct sd_bus *bus;
	int rc;

	/* get the query */
	rc = afb_req_param_convert(req, 0, AFB_PREDEFINED_TYPE_JSON_C, &first_arg);
	if (rc < 0)
		goto bad_request;
	obj = (struct json_object*)afb_data_ro_pointer(first_arg);
	if (obj == NULL)
		goto bad_request;

	/* get parameters */
	args = NULL;
	json_object_object_get_ex(obj, "data", &args);
	destination = strval(obj, "destination", NULL);
	path        = strval(obj, "path",      NULL);
	interface   = strval(obj, "interface", NULL);
	member      = strval(obj, "member",    NULL);
	signature   = strval(obj, "signature", "");
	busname     = strval(obj, "bus",       NULL);

	/* check parameters */
	if (path == NULL || member == NULL)
		goto bad_request;
	busname = std_busname(busname);
	if (busname == NULL)
		goto bad_request;
	bus = getbus(busname);
	if (bus == NULL)
		goto internal_error;
	if (bus == NULL)
		goto internal_error;

	/* creates the message */
	rc = sd_bus_message_new_signal(bus, &msg, path, interface, member);
	if (rc < 0)
		goto internal_error;
	if (destination != NULL) {
		rc = sd_bus_message_set_destination(msg, destination);
		if (rc < 0)
			goto internal_error;
	}
	rc = jsonc2msg(msg, signature, args);
	if (rc < 0)
		goto bad_request;

	/* Send the message */
	rc = sd_bus_send(bus, msg, NULL);
	if (rc < 0)
		goto internal_error;
	afb_req_reply(req, 0, 0, NULL);
	goto cleanup;

internal_error:
	afb_req_reply(req, AFB_ERRNO_INTERNAL_ERROR, 0, NULL);
	goto cleanup;

bad_request:
	afb_req_reply(req, AFB_ERRNO_INVALID_REQUEST, 0, NULL);

cleanup:
	sd_bus_message_unref(msg);
}

/*****************************************************************************************/
/* manage calls */
/*****************************************************************************************/

/*
 * handle the reply
 */
static int on_call_reply(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
	afb_req_t req = userdata;
	struct json_object *obj = NULL;
	afb_data_t data;
	int rc;
	int sts = AFB_ERRNO_GENERIC_FAILURE;
	const sd_bus_error *err;

	/* make the reply */
	err = sd_bus_message_get_error(msg);
	if (err != NULL)
		obj = jsonc_of_dbus_error(err);
	else {
		rc = msg2jsonc(msg, &obj);
		if (rc < 0)
			obj = NULL;
		else
			sts = 0;
	}

	/* send the reply now */
	afb_create_data_raw(&data, AFB_PREDEFINED_TYPE_JSON_C, obj, 0, (void*)json_object_put, obj);
	afb_req_reply(req, sts, 1, &data);
	afb_req_unref(req);
	return 1;
}

/* process signal requests */
static void process_call(afb_req_t req)
{
	afb_data_t first_arg;
	struct json_object *obj;
	struct json_object *args;

	const char *busname;
	const char *destination;
	const char *path;
	const char *interface;
	const char *member;
	const char *signature;

	struct sd_bus_message *msg = NULL;
	struct sd_bus *bus;
	int rc;

	/* get the query */
	rc = afb_req_param_convert(req, 0, AFB_PREDEFINED_TYPE_JSON_C, &first_arg);
	if (rc < 0)
		goto bad_request;
	obj = (struct json_object*)afb_data_ro_pointer(first_arg);
	if (obj == NULL)
		goto bad_request;

	/* get parameters */
	args = NULL;
	json_object_object_get_ex(obj, "data", &args);
	destination = strval(obj, "destination", NULL);
	path        = strval(obj, "path",      NULL);
	interface   = strval(obj, "interface", NULL);
	member      = strval(obj, "member",    NULL);
	signature   = strval(obj, "signature", "");
	busname     = strval(obj, "bus",       NULL);

	/* check parameters */
	if (path == NULL || member == NULL)
		goto bad_request;
	busname = std_busname(busname);
	if (busname == NULL)
		goto bad_request;
	bus = getbus(busname);
	if (bus == NULL)
		goto internal_error;
	if (bus == NULL)
		goto internal_error;

	/* creates the message */
	rc = sd_bus_message_new_method_call(bus, &msg, destination, path, interface, member);
	if (rc != 0)
		goto internal_error;
	rc = jsonc2msg(msg, signature, args);
	if (rc < 0)
		goto bad_request;

	/* Send the message */
	rc = sd_bus_call_async(bus, NULL, msg, on_call_reply, afb_req_addref(req), -1);
	if (rc < 0)
		goto internal_error;
	goto cleanup;

internal_error:
	afb_req_reply(req, AFB_ERRNO_INTERNAL_ERROR, 0, NULL);
	goto cleanup;

bad_request:
	afb_req_reply(req, AFB_ERRNO_INVALID_REQUEST, 0, NULL);

cleanup:
	sd_bus_message_unref(msg);
}

/*****************************************************************************************/
/* verbs */
/*****************************************************************************************/

static void v_call(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	submit(req, process_call);
}

static void v_signal(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	submit(req, process_signal);
}

static void v_subscribe(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	submit(req, process_subscribe);
}

static void v_unsubscribe(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	submit(req, process_unsubscribe);
}

static void v_nfc_check(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	afb_timer_t timer_nfc_check;

	int location_timer;

	afb_req_subscribe(req, event_nfc);

	// every 5 seconds, an event is sent to the listeners (e.g. display-binding)
	location_timer = afb_timer_create(&timer_nfc_check, 0, 0, 0, 0, 5000, 0, check_nfc_cb, NULL, 0);
    
	if (location_timer < 0)
    {
        AFB_NOTICE("Timer launch fail");
    }

	afb_req_reply(req, 0, 0, NULL);
}

static void v_version(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	afb_data_t data;
	afb_create_data_raw(&data, AFB_PREDEFINED_TYPE_STRINGZ, VERSION, sizeof VERSION, NULL, NULL);
	afb_req_reply(req, 0, 1, &data);
}

static void v_info(afb_req_t req, unsigned narg, const afb_data_t args[])
{
	afb_req_reply(req, 0, 0, NULL);
}

/* array of the verbs exported to afb-daemon */
static const afb_verb_t verbs[] = {
  { .verb="version",       .callback=v_version,     .info="get cuurent version" },
  { .verb="call",          .callback=v_call,        .info="call to dbus method" },
  { .verb="signal",        .callback=v_signal,      .info="signal to dbus method" },
  { .verb="subscribe",     .callback=v_subscribe,   .info="subscribe to a dbus signal" },
  { .verb="unsubscribe",   .callback=v_unsubscribe, .info="unsubscribe to a dbus signal" },
  { .verb="subscribe_nfc", .callback=v_nfc_check,   .info="subscribe to the nfc check" },
  { .verb="info",          .callback=v_info,        .info="info of all verbs" },
  { .verb=NULL }
};

/*****************************************************************************************/
/* initialisation and declaration */
/*****************************************************************************************/

/* instanciate the default event */
static int create_default_event(afb_api_t api)
{
	struct evrec *evrec = create_evrec(api, DEFAULT_EVENT_NAME);
	if (evrec == NULL)
		return -1;
	evrec->refcnt = 1;
	return 0;
}

/* initialisation */
static int mainctl(afb_api_t api, afb_ctlid_t ctlid, afb_ctlarg_t ctlarg, void *userdata)
{
	int rc = 0;
	pthread_t thread;
	switch (ctlid) {
	case afb_ctlid_Pre_Init:
		/* create the default event */
		rc = create_default_event(api);
		/* create the loop signaler */
		if (rc >= 0)
			rc = efd = eventfd(0, 0);
		/* start the thread */
		if (rc >= 0)
			rc = pthread_create(&thread, NULL, run, NULL);
		break;
	case afb_ctlid_Init:
		rc = afb_api_new_event(api, " NFC event - the device exists", &event_nfc);
		break;
	default:
		break;
	}
	return rc;
}

/* declaration of the binding for afb-binder */
const afb_binding_t afbBindingExport =
{
    .api   = "dbus",
    .info  = "dbus binding",
    .mainctl = mainctl,
    .verbs = verbs
};

