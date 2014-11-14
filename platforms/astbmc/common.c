/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <skiboot.h>
#include <device.h>
#include <console.h>
#include <psi.h>
#include <chip.h>
#include <xscom.h>
#include <ast.h>
#include <ipmi.h>
#include <bt.h>

#include "astbmc.h"

/* UART1 config */
#define UART_IO_BASE	0x3f8
#define UART_IO_COUNT	8
#define UART_LPC_IRQ	4

/* BT config */
#define BT_IO_BASE	0xe4
#define BT_IO_COUNT	3
#define BT_LPC_IRQ	10

void astbmc_ext_irq(unsigned int chip_id __unused)
{
	uart_irq();
	bt_irq();
}

void astbmc_init(void)
{
	/* Initialize PNOR/NVRAM */
	pnor_init();

	/* Register the BT interface with the IPMI layer */
	bt_init();
	ipmi_rtc_init();
	ipmi_opal_init();

	/* As soon as IPMI is up, inform BMC we are in "S0" */
	ipmi_set_power_state(IPMI_PWR_SYS_S0_WORKING, IPMI_PWR_NOCHANGE);

	/* Setup UART console for use by Linux via OPAL API */
	if (!dummy_console_enabled())
		uart_setup_opal_console();
}

int64_t astbmc_ipmi_power_down(uint64_t request)
{
	if (request != IPMI_CHASSIS_PWR_DOWN) {
		prlog(PR_WARNING, "PLAT: unexpected shutdown request %llx\n",
				   request);
	}

	return ipmi_chassis_control(request);
}

int64_t astbmc_ipmi_reboot(void)
{
	return ipmi_chassis_control(IPMI_CHASSIS_HARD_RESET);
}

static void astbmc_fixup_dt_bt(struct dt_node *lpc)
{
	struct dt_node *bt;
	char namebuf[32];

	/* First check if the BT interface is already there */
	dt_for_each_child(lpc, bt) {
		if (dt_node_is_compatible(bt, "bt"))
			return;
	}

	sprintf(namebuf, "ipmi-bt@i%x", BT_IO_BASE);
	bt = dt_new(lpc, namebuf);

	dt_add_property_cells(bt, "reg",
			      1, /* IO space */
			      BT_IO_BASE, BT_IO_COUNT);
	dt_add_property_strings(bt, "compatible", "ipmi-bt");

	/* Mark it as reserved to avoid Linux trying to claim it */
	dt_add_property_strings(bt, "status", "reserved");
}

static void astbmc_fixup_dt_uart(struct dt_node *lpc)
{
	/*
	 * The official OF ISA/LPC binding is a bit odd, it prefixes
	 * the unit address for IO with "i". It uses 2 cells, the first
	 * one indicating IO vs. Memory space (along with bits to
	 * represent aliasing).
	 *
	 * We pickup that binding and add to it "2" as a indication
	 * of FW space.
	 */
	struct dt_node *uart;
	char namebuf[32];

	/* First check if the UART is already there */
	dt_for_each_child(lpc, uart) {
		if (dt_node_is_compatible(uart, "ns16550"))
			return;
	}

	/* Otherwise, add a node for it */
	sprintf(namebuf, "serial@i%x", UART_IO_BASE);
	uart = dt_new(lpc, namebuf);

	dt_add_property_cells(uart, "reg",
			      1, /* IO space */
			      UART_IO_BASE, UART_IO_COUNT);
	dt_add_property_strings(uart, "compatible",
				"ns16550",
				"pnpPNP,501");
	dt_add_property_cells(uart, "clock-frequency", 1843200);
	dt_add_property_cells(uart, "current-speed", 115200);

	/*
	 * This is needed by Linux for some obscure reasons,
	 * we'll eventually need to sanitize it but in the meantime
	 * let's make sure it's there
	 */
	dt_add_property_strings(uart, "device_type", "serial");

	/*
	 * Add interrupt. This simulates coming from HostBoot which
	 * does not know our interrupt numbering scheme. Instead, it
	 * just tells us which chip the interrupt is wired to, it will
	 * be the PSI "host error" interrupt of that chip. For now we
	 * assume the same chip as the LPC bus is on.
	 */
	dt_add_property_cells(uart, "ibm,irq-chip-id", dt_get_chip_id(lpc));
}

static struct dt_node *dt_create_i2c_master(struct dt_node *n, uint32_t eng_id)
{
	struct dt_node *i2cm;

	/* Each master registers set is of length 0x20 */
	i2cm = dt_new_addr(n, "i2cm", 0xa0000 + eng_id * 0x20);
	if (!i2cm)
		return NULL;

	dt_add_property_string(i2cm, "compatible",
			       "ibm,power8-i2cm");
	dt_add_property_cells(i2cm, "reg", 0xa0000 + eng_id * 0x20,
			      0x20);
	dt_add_property_cells(i2cm, "clock-frequency", 50000000);
	dt_add_property_cells(i2cm, "chip-engine#", eng_id);
	dt_add_property_cells(i2cm, "#address-cells", 1);
	dt_add_property_cells(i2cm, "#size-cells", 0);

	return i2cm;
}

static struct dt_node *dt_create_i2c_bus(struct dt_node *i2cm, const char *port_name,
					 uint32_t port_id)
{
	static struct dt_node *port;

	port = dt_new_addr(i2cm, "i2c-bus", port_id);
	if (!port)
		return NULL;

	dt_add_property_strings(port, "compatible",
				"ibm,power8-i2c-port", "ibm,opal-i2c");
	dt_add_property_string(port, "ibm,port-name", port_name);
	dt_add_property_cells(port, "reg", port_id);
	dt_add_property_cells(port, "bus-frequency", 400000);
	dt_add_property_cells(port, "#address-cells", 1);
	dt_add_property_cells(port, "#size-cells", 0);

	return port;
}

static struct dt_node *dt_create_i2c_device(struct dt_node *bus, uint8_t addr,
					    const char *name, const char *compat,
					    const char *label)
{
	struct dt_node *dev;

	dev = dt_new_addr(bus, name, addr);
	if (!dev)
		return NULL;

	dt_add_property_string(dev, "compatible", compat);
	dt_add_property_string(dev, "label", label);
	dt_add_property_cells(dev, "reg", addr);
	dt_add_property_string(dev, "status", "ok");

	return dev;
}

static void astbmc_fixup_dt_i2cm(void)
{
	struct proc_chip *c;
	struct dt_node *master, *bus;
	char name[32];

	/*
	 * Look if any i2c is in the device-tree, in which
	 * case we assume HB did the job
	 */
	if (dt_find_compatible_node(dt_root, NULL, "ibm,power8-i2cm"))
		return;

	/* Create nodes for i2cm1 of chip 0 */
	c = get_chip(0);
	assert(c);

	master = dt_create_i2c_master(c->devnode, 1);
	assert(master);
	sprintf(name,"p8_%08x_e%dp%d", c->id, 1, 0);
	bus = dt_create_i2c_bus(master, name, 0);
	assert(bus);
	sprintf(name,"p8_%08x_e%dp%d", c->id, 1, 2);
	bus = dt_create_i2c_bus(master, name, 2);
	assert(bus);
	dt_create_i2c_device(bus, 0x50, "eeprom", "atmel,24c64", "system-vpd");
	assert(bus);
}

static void astbmc_fixup_dt(void)
{
	struct dt_node *n, *primary_lpc = NULL;

	/* Find the primary LPC bus */
	dt_for_each_compatible(dt_root, n, "ibm,power8-lpc") {
		if (!primary_lpc || dt_has_node_property(n, "primary", NULL))
			primary_lpc = n;
		if (dt_has_node_property(n, "#address-cells", NULL))
			break;
	}

	if (!primary_lpc)
		return;

	/* Fixup the UART, that might be missing from HB */
	astbmc_fixup_dt_uart(primary_lpc);

	/* BT is not in HB either */
	astbmc_fixup_dt_bt(primary_lpc);

	/* Add i2c masters if needed */
	astbmc_fixup_dt_i2cm();
}

static void astbmc_fixup_psi_bar(void)
{
	struct proc_chip *chip = next_chip(NULL);
	uint64_t psibar;

	/* Read PSI BAR */
	if (xscom_read(chip->id, 0x201090A, &psibar)) {
		prerror("PLAT: Error reading PSI BAR\n");
		return;
	}
	/* Already configured, bail out */
	if (psibar & 1)
		return;

	/* Hard wire ... yuck */
	psibar = 0x3fffe80000001;

	printf("PLAT: Fixing up PSI BAR on chip %d BAR=%llx\n",
	       chip->id, psibar);

	/* Now write it */
	xscom_write(chip->id, 0x201090A, psibar);
}

void astbmc_early_init(void)
{
	/* Hostboot's device-tree isn't quite right yet */
	astbmc_fixup_dt();

	/* Hostboot forgets to populate the PSI BAR */
	astbmc_fixup_psi_bar();

	/* Send external interrupts to me */
	psi_set_external_irq_policy(EXTERNAL_IRQ_POLICY_SKIBOOT);

	/* Initialize AHB accesses via AST2400 */
	ast_io_init();

	/*
	 * Depending on which image we are running, it may be configuring
	 * the virtual UART or not. Check if VUART is enabled and use
	 * SIO if not. We also correct the configuration of VUART as some
	 * BMC images don't setup the interrupt properly
	 */
	if (ast_is_vuart1_enabled()) {
		printf("PLAT: Using virtual UART\n");
		ast_disable_sio_uart1();
		ast_setup_vuart1(UART_IO_BASE, UART_LPC_IRQ);
	} else {
		printf("PLAT: Using SuperIO UART\n");
		ast_setup_sio_uart1(UART_IO_BASE, UART_LPC_IRQ);
	}

	/* Similarily, some BMCs don't configure the BT interrupt properly */
	ast_setup_ibt(BT_IO_BASE, BT_LPC_IRQ);

	/* Setup UART and use it as console with interrupts */
	uart_init(true);
}
