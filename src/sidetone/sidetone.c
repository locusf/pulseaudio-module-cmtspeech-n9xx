/*
 * Copyright (C) 2010 Nokia Corporation.
 *
 * Contact: Maemo MMF Audio <mmf-audio@projects.maemo.org>
 *          or Jyri Sarha <jyri.sarha@nokia.com>
 *
 * These PulseAudio Modules are free software; you can redistribute
 * it and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/call-state-tracker.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>
#include <pulsecore/mutex.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/modargs.h>

#include <pulsecore/modules/alsa/alsa-mixer.h>
#include <pulsecore/modules/alsa/alsa-util-old.h>
#include <pulsecore/modules/alsa/alsa-sink-old.h>

#include "ctrl-element.h"
#include "alsa-utils.h"
#include "sidetone-args.h"
#include "sidetone.h"

/* we have only 7 steps in Left Digital Loopback element i.e. sidetone control element*/
#define MAX_SIDETONE_STEP 7

/* Describes the volume of an element in the sideone path. */
struct element_volume {
    long volume;
    /* Sidetone is mono, so we're only interested in one channel (default SND_MIXER_SCHN_MONO) */
    snd_mixer_selem_channel_id_t channel;
    /* Is this a playback or capture element? */
    pa_bool_t playback;
};

/* Sidetone object data */
struct sidetone {
    /* The mixer that contains the elements of the sidetone loop */
    snd_mixer_t *mixer;
    /* The element used for controlling the sidetone loop volume */
    ctrl_element *ctrl_element;
    /* If important sinks  unlinked unexpectedly, paths owned by
     * them are destroyed. This flag is set in such cases to disable the
     * sidetone altogether. */
    pa_bool_t dead;
    /* The sinks whose element paths we want to monitor */
    pa_sink **sinks ;
    int num_sinks;
    /* Sink unlink slot is needed in case important sinks disappear */
    pa_hook_slot* sink_unlink_slot;
    /* The callbacks implemented by the sidetone object are called from both
     * the main thread as well as the IO threads of ALSA sinks and sources.
     * Thus, we need to lock this mutex whenever we are touching the internals
     * of the sidetone object inside callbacks. */
    pa_mutex *mutex;
    /* subscription for getting current volume*/
    pa_subscription *sink_subscription;
    /* sink we are interested in*/
    pa_sink *master_sink;
    /* store the current volume , to compare next time */
    pa_cvolume *volume_current;
    /* total possible steps in main-volume */
    struct mv_volume_steps *total_steps;
    /* step used for setting sidetone control element volume */
    int sidetone_step;

};



static int sidetone_volume_get_step(struct sidetone *st){

    pa_cvolume *cvol;
    pa_volume_t volume;
    int volume_mb;
    int i=0;

    cvol = &st->master_sink->real_volume;

    if(st->volume_current){
        if(pa_cvolume_equal(cvol,st->volume_current)){
            pa_log_debug("volume is same as previous one");
            return -1;
        }
    }

    st->volume_current->channels=cvol->channels;
    for(i=0;i < st->volume_current->channels;i++){
        st->volume_current->values[i] = cvol->values[i];
    }

    volume = pa_cvolume_avg(cvol);
    pa_log_debug("volume %d from sink %s",volume,st->master_sink->name);

    volume_mb=(int)(pa_sw_volume_to_dB(volume)*100 - 0.5);
    pa_log_debug("volume %d milibels ",volume_mb);

    /* now map the sidetone step*/

    for(i=0;i<st->total_steps->n_steps;i++){
       if(st->total_steps->step[i]==volume_mb){
          st->total_steps->current_step=i;
          st->sidetone_step=(int)(st->total_steps->n_steps - 1 - st->total_steps->current_step);
          if( st->sidetone_step > MAX_SIDETONE_STEP)
             st->sidetone_step= MAX_SIDETONE_STEP;
       }
    }

    return st->sidetone_step;

}

/* get the current volume , convert it into milibels , find out the corrosponding volume step , map this
volume step to sidetone control element volume step. set the sidetone volume using control element volume
step */
static void sink_input_subscribe_sidetone_cb(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, sidetone *st) {
    int sidetone_step=-1;
    int ret = 0;
    pa_assert(c);
    pa_assert(st);

    pa_log_debug("subscription event is called  ");

    if (PA_SUBSCRIPTION_EVENT_SINK_INPUT != (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)){
        pa_log_debug("subscription event not found");
        return;
    }

    /* fetch the sidetone step */
    sidetone_step = sidetone_volume_get_step(st);
    if(sidetone_step < 0){
        pa_log_debug("cant fetch the correct sidetone volume step");
    }

    pa_log_debug(" setting sidetone setp %d",sidetone_step);

    /* set the sidetone volume using step */
    ret = set_ctrl_element_volume(st->ctrl_element,sidetone_step);
    if(ret < 0){
       pa_log_debug("can't set sidetone volume");
    }

}


/* Sets sidetone in a disabled state and mutes the control element. Only used
 * in exceptional circumstances where an important sink or source is unlinked
 * unexpectedly. */
static void set_dead(sidetone *st) {
    pa_assert(st);

    st->dead = TRUE;
    ctrl_element_mute(st->ctrl_element);
}


/* Called from the main thread
 * If an alsa sink whose paths we're monitoring is unlinked, we need to disable
 * sidetone, because the paths associated with the sink are no longer valid. */
static pa_hook_result_t sink_unlink_cb(pa_sink *sink, void *call_data, sidetone *st) {
    pa_assert(sink);
    pa_assert(st);
    int i;
    pa_log_debug(" sink_unlink_cb");
    pa_mutex_lock(st->mutex);

    for(i = 0; i < st->num_sinks; i++) {
        if(sink == st->sinks[i]) {
            pa_log_warn("An important sink (%s) was unlinked. Disabling sidetone module.", sink->name);
            set_dead(st);
            break;
        }
    }

    pa_mutex_unlock(st->mutex);

    return PA_HOOK_OK;
}



/* Create a new sidetone instance. argument contains the raw module args of the sidetone module */
sidetone *sidetone_new(pa_core *core, const char* argument) {
    pa_assert(core);
    pa_assert(argument);
    sidetone *st = NULL;
    sidetone_args *st_args = NULL;
    int i = 0;


    if(!(st_args = sidetone_args_new(argument))) {
        goto fail;
    }

    st = pa_xnew0(struct sidetone, 1);
    st->volume_current=pa_xnew0(struct pa_cvolume, 1);;

    st->total_steps=pa_xnew0(struct mv_volume_steps,1);
    st->total_steps->n_steps= st_args->steps->n_steps;
    st->total_steps->current_step=st_args->steps->current_step;
    for(i=0;i<st->total_steps->n_steps;i++){
        st->total_steps->step[i] = st_args->steps->step[i];
    }

    st->mutex = pa_mutex_new(FALSE, FALSE);

    if(!(st->mixer = pa_alsa_old_open_mixer(st_args->mixer))) {
        pa_log_error("Failed to open mixer \"%s\"", st_args->mixer);
        goto fail;
    }

   if(!(st->ctrl_element = ctrl_element_new(st->mixer, st_args->control_element))) {
        pa_log_error("Failed to create control element");
        goto fail;
    }

    /* there should be at least one sink */
    pa_assert(st_args->num_sinks > 0);
    st->sinks = pa_xmalloc0(st_args->num_sinks * sizeof(pa_sink*));
    st->num_sinks = st_args->num_sinks;
    for(i = 0; i < st_args->num_sinks; i++) {
        pa_sink *sink = NULL;

        if(!(sink = pa_namereg_get(core,st_args->sinks[i], PA_NAMEREG_SINK))) {
            pa_log_error("Sink %s not found", st_args->sinks[i]);
            goto fail;
        }
        st->master_sink=sink;
        st->sinks[i] = sink;
    }


    /* Attach to sink/source unlink hooks so that we can react if important sinks/sources are unlinked */
    st->sink_unlink_slot = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_NORMAL,
                                           (pa_hook_cb_t)sink_unlink_cb, st);


    /* subscription made for fetching the current main volume */
    st->sink_subscription = pa_subscription_new(core, PA_SUBSCRIPTION_MASK_SINK_INPUT , sink_input_subscribe_sidetone_cb, st);

    st->dead = FALSE;


    sidetone_args_free(st_args);

    pa_log_debug("sidetone initialization is done successfully");

    return st;

fail:

    if(st_args)
        sidetone_args_free(st_args);

    if(st)
        sidetone_free(st);

    return NULL;
}

void sidetone_free(sidetone *st) {
    pa_assert(st);
    pa_hook_slot *slot=NULL;
    int i = 0;


    ctrl_element_mute(st->ctrl_element);

    if(st->total_steps){
        pa_xfree(st->total_steps);
        st->total_steps=NULL;
    }

    if(st->volume_current){
        pa_xfree(st->volume_current);
        st->volume_current=NULL;
    }

    if(st->sink_subscription){
        pa_subscription_free(st->sink_subscription);
        st->sink_subscription=NULL;
    }

    if(st->ctrl_element)
        ctrl_element_free(st->ctrl_element);


    if(st->sink_unlink_slot){
        pa_hook_slot_free(st->sink_unlink_slot);
        st->sink_unlink_slot=NULL;
    }
    if(st->sinks){
        pa_xfree(st->sinks);
        st->sinks=NULL;
    }

    if(st->mutex)
        pa_mutex_free(st->mutex);

    pa_xfree(st);

    pa_log_debug(" sidetone freed  " );
}

