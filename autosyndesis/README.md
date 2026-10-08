# ATSSS with Femto

**A**u**T**o**S**ynde**S**i**S** is an ATSSS-like path selection C implementation collection for Femto-QUIC (based on PicoQUIC) and similar network libraries.

The following modes are currently implemented:

Mode                 | Description                                   | Parameters
---------------------|-----------------------------------------------|-----------------
Active-Standby       | Use the default path unless it fails          | default link, link down threshold
MinRTT               | Use the path with the lowest latency          | switch margin
Round Robin          | Change paths rapidly, depending on the share  | initial path, share
Duplication          | Send data over all paths                      | - (see MinRTT for Opp.Redundant)

## Usage Examples

See `example.c` for real examples.

### Active-Standby (Pseudo-C-Code)

It will try to use the `default_link` all the time. If the link is down (`rtt > LINK_DOWN_THRESHOLD`), it will try to switch to the next path available, if it is up.

```c
autosyndesis_state_t atsss_state;
autosyndesis_actv_stdby_props_t atsss_actv_stdby_props;
...
atsss_actv_stdby_props.down_threshold = LINK_DOWN_THRESHOLD;
atsss_actv_stdby_props.default_link = 0;
autosyndesis_set_mode(&atsss_state, MODE_ACTIVE_STANDBY, &atsss_actv_stdby_props);
...
int next_path = autosyndesis_determine_path(&atsss_state);
send_data_mp(&mp_client, ..., ..., next_path);
...
packet_received {
    rtt = calculate_rtt();
    autosyndesis_update_link_properties(&atsss_state, i, rtt);
}
```

### Smallest Delay / minRTT

It will use the link with the smallest delay.

```c
autosyndesis_state_t atsss_state;
...
autosyndesis_set_mode(&atsss_state, MODE_SMALLEST_DELAY, NULL);
...
int next_path = autosyndesis_determine_path(&atsss_state);
send_data_mp(&mp_client, ..., ..., next_path);
...
packet_received {
    rtt = calculate_rtt();
    autosyndesis_update_link_properties(&atsss_state, i, rtt);
}
```

### Load-Balancing (Static, Round-Robin)

A static share can be defined for each path. For example, with two paths and a share of `{ 5, 3 }`, 5 packets are sent over path 0, 3 packets are sent over path 1 in a random order. The order of next paths may be `{ 0, 0, 1, 0, 0, 1 }` for instance.


```c
autosyndesis_state_t atsss_state;
autosyndesis_lb_rr_props_t atsss_lb_rr_props = { 0 };
...
atsss_lb_rr_props.share[0] = 5;  // share needs to be defined for each path
atsss_lb_rr_props.share[1] = 3;
atsss_lb_rr_props.initial_path = 0;  // path to start with
autosyndesis_set_mode(&atsss_state, MODE_LOAD_BALANCING, &atsss_lb_rr_props);
...
int next_path = autosyndesis_determine_path(&atsss_state);
send_data_mp(&mp_client, ..., ..., next_path);
```


## Duplication / Opportunistic Redundant

Packets are sent over all paths simultaneously. In case of retransmission, the MinRTT algorithm is used.

```c
autosyndesis_state_t atsss_state;
autosyndesis_lb_rr_props_t atsss_lb_rr_props = { 0 };
...
atsss_lb_rr_props.share[0] = 5;  // share needs to be defined for each path
atsss_lb_rr_props.share[1] = 3;
autosyndesis_set_mode(&atsss_state, MODE_LOAD_BALANCING, &atsss_lb_rr_props);
...
if (atsss_state.current_mode == MODE_MP_DUPLICATION && !is_retransmission()) {
    for (int p = 0; p < atsss_state.num_paths; p++) {
        send_data_mp(&mp_client, ..., ..., p);
    }
} else {
    int next_path = autosyndesis_determine_path(&atsss_state);
    send_data_mp(&mp_client, ..., ..., next_path);
}
```


## Building

```bash
mkdir build && cd build

export FEMTO_QUIC_DIR=...
export PICOQUIC_DIR=...
export PICOTLS_DIR=...

cmake ..
make

# Building AuToSyndeSiS without Femto/PicoQUIC is possible
# as it is only needed for the example executable
cmake -DBUILD_EXAMPLE=OFF ..
make
```
