# mod_free_amd

Answering Machine Detection Freeswitch module, that tries to use the same APP interface and Config parameters used by commercial module - mod_com_amd

- [mod\_free\_amd](#mod_free_amd)
  - [Context](#context)
  - [Config File](#config-file)
    - [Example](#example)
  - [APP Interface](#app-interface)
    - [Commands](#commands)
    - [Dialplan Example](#dialplan-example)
    - [Lua Example](#lua-example)
  - [Results](#results)
  - [Available versions](#available-versions)
    - [Create your own packages](#create-your-own-packages)
  - [Future Work](#future-work)
  - [More details](#more-details)

## Context

mod_free_amd is a Freeswitch module that provides Answering Machine Detection (AMD) capabilities. It is designed to be a free alternative to the commercial mod_com_amd module, allowing users to implement AMD functionality in their Freeswitch installations without incurring additional costs.

The implementation tries to emulate the same APP interface and configuration parameters as mod_com_amd, making it easier for users to switch between the two modules without significant changes to their existing setups.

The interpretation done results from a personal reverse engineering effort and might not be 100% accurate when compared to the commercial module.

The behavior might differ in some edge cases, because the code results from a user interpretation of the documentation provided and some guesswork about the parameters behavior.

The module is provided as-is, without any guarantees regarding its performance or accuracy.

## Config File

Uses exactly the same configuration file as mod_com_amd: `amd.conf.xml`.
Also uses the exact same parameters and default values.

### Example

```xml
<configuration name="amd.conf" description="AMD Configuration">
  <!-- AMD -->
  <settings>
<!-- silent_threshold: The level of volume to consider talking or not talking, same scale as used in mod_conference -->
    <param name="silent_threshold" value="256"/>
<!-- silent_initial: Time in ms for there to be silence after answer in order to result in "silent-initial" with status of person -->
    <param name="silent_initial" value="4500"/>
<!-- silent_after_intro: Time in ms after an initial non silent greeting in order to result in silent-after-intro with status of person -->
    <param name="silent_after_intro" value="1000"/>
<!-- silent_max_session: Time in ms of total silence before we allow detection to complete -->
    <param name="silent_max_session" value="200"/>
<!-- noise_max_intro: Time in ms length of initial intro over which in order to result in max-intro with status of person -->
    <param name="noise_max_intro" value="1250"/>
<!-- noise_min_length: Time in ms minimum to be considered a word  -->
    <param name="noise_min_length" value="120"/>
<!-- noise_inter_silence: Time in ms of silence to be considered a word break -->
    <param name="noise_inter_silence" value="30"/>
<!-- noise_max_count: If we have more than this many noise hits (words) result will be "max-count" with status of machine -->
    <param name="noise_max_count" value="6"/>
<!-- total_analysis_time: total time in ms that we will try to analyze a call -->
    <param name="total_analysis_time" value="5000"/>
<!-- debug: set to 1 to get more debug information -->
    <param name="debug" value="1"/>
  </settings>
</configuration>
```

## APP Interface

The module provides a similar APP interface, without any changes.

So, we can start, stop and wait for a result, using the same commands as mod_com_amd.

### Commands

- voice_start
- voice_stop
- waitforresult

### Dialplan Example

```xml
<action application="voice_start"/>
<action application="voice_stop"/>
<action application="waitforresult"/>
```

### Lua Example

```lua
session:execute("voice_start", voice_start_args)
session:execute("voice_stop")
```

## Results

The current module was tested on multiple audios and correctly identified the results in most cases.
But, the samples used were not extensive enough to guarantee 100% accuracy.

## Available versions

- CentOS 7:
  - 1.10.9;
- Debian - Bookworm:
  - 1.10.12;
- Debian - Bullseye:
  - 1.10.12;

### Create your own packages

There are available github actions to create your own packages for different Freeswitch versions and OS distributions (Debian and CentOS 7).

You can find the github actions [workflows](.github/workflows).

Go to the github repo, and inside actions sections, select the flows:

- Build DEB Package (Manual);
- Build RPM Package (Manual);

Then, click on "Run workflow", update the optional arguments with the values you want, and click on "Run workflow" button.

On successful completion of the workflow, you can download the created package from the "Artifacts" section. Click on the link, and a download will start.

## Future Work

- More extensive testing and validation against mod_com_amd to ensure accuracy and reliability.
- Some improvements on APP interface, more test is required;
- Add metrics;

## More details

[mod_com_amd](https://developer.signalwire.com/freeswitch/FreeSWITCH-Explained/Modules/mod_com_amd_4653131/)
