/*
 * Copyright (C) 2015 Andrea Mazzoleni
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
 */

#include "portable.h"

#include "support.h"
#include "state.h"
#include "raid/raid.h"

/**
 * Annual Failure Rate data points from Backblaze.
 *
 * From:
 * https://www.backblaze.com/blog-smart-stats-2014-8.html
 */
struct afr_point {
	uint64_t value; /**< Value of the SMART raw attribute. */
	double afr; /**< Annual Failure Rate at this value. */
};

static struct afr_point AFR_5[] = {
	{ 0, 0 },
	{ 1, 0.027432608477803388 },
	{ 4, 0.07501976284584981 },
	{ 16, 0.23589260654405794 },
	{ 70, 0.36193219378600433 },
	{ 260, 0.5676621428968173 },
	{ 1100, 1.5028253400346423 },
	{ 4500, 2.0659987547404763 },
	{ 17000, 1.7755385684503124 },
	{ 0, 0 }
};

static struct afr_point AFR_187[] = {
	{ 0, 0 },
	{ 1, 0.33877621175661743 },
	{ 3, 0.5014425058387142 },
	{ 11, 0.5346094598348444 },
	{ 20, 0.8428063943161636 },
	{ 35, 1.4429071005017484 },
	{ 65, 1.6190935390549661 },
	{ 0, 0 }
};

static struct afr_point AFR_188[] = {
	{ 0, 0 },
	{ 1, 0.10044174089362015 },
	{ 13000000000ULL, 0.334030592234279 },
	{ 26000000000ULL, 0.36724705400842445 },
	{ 0, 0 }
};

static struct afr_point AFR_193[] = {
	{ 0, 0 },
	{ 1300, 0.024800489215129725 },
	{ 5500, 0.05859661417772557 },
	{ 21000, 0.19566577603409208 },
	{ 90000, 0.2673688205712117 },
	{ 0, 0 }
};

static struct afr_point AFR_197[] = {
	{ 0, 0 },
	{ 1, 0.34196613799103254 },
	{ 2, 0.6823772508117681 },
	{ 16, 0.9564879341127684 },
	{ 40, 1.6519989942167461 },
	{ 100, 2.5137741046831956 },
	{ 250, 3.3203378817413904 },
	{ 0, 0 }
};

static struct afr_point AFR_198[] = {
	{ 0, 0 },
	{ 1, 0.8135764944275583 },
	{ 2, 1.1173469387755102 },
	{ 4, 1.3558692421991083 },
	{ 10, 1.7464114832535886 },
	{ 12, 2.6449275362318843 },
	{ 0, 0 }
};

/**
 * Computes the estimated AFR (Annual Failure Rate) from a set of data points.
 */
static double smart_afr_value(struct afr_point* tab, uint64_t value)
{
	unsigned i;
	double delta_afr;
	double delta_value;

	/* if first point */
	if (value == 0)
		return 0;

	i = 1;
	while (tab[i].value != 0 && tab[i].value < value)
		++i;

	/* if last point */
	if (tab[i].value == 0)
		return tab[i-1].afr;

	/* if exact value */
	if (tab[i].value == value)
		return tab[i].afr;

	delta_afr = tab[i].afr - tab[i-1].afr;
	delta_value = tab[i].value - tab[i-1].value;

	/* linear interpolation between the two points */
	return tab[i-1].afr + (value - tab[i-1].value) * delta_afr / delta_value;
}

/**
 * Computes the estimated AFR of a set of SMART attributes.
 *
 * We assume the AFR (Annual Failure Rate) data from Backblaze
 * defined as AFR = 8760/MTBF (Mean Time Between Failures in hours).
 *
 * This is consistent with their definition of AFR:
 * "An annual failure rate of 100% means that if you have one disk drive slot
 * and keep a drive running in it all the time, you can expect an average of
 * one failure a year. If, on the other hand, you have one failure per month
 * in your one drive slot, then your failure rate is 1200%. If you run n
 * drives for t years with an annual failure rate of r, the number of failures
 * is expected to be n * r * t."
 *
 * Note that this definition is different from the one given
 * by Seagate, that defines AFR = 1 - exp(-8760/MTBF), that
 * instead represents the probability of a failure in the next
 * year, and then we call it as AFP (Annual Failure Probability).
 *
 * To combine the different AFR from different SMART attributes,
 * we sums them as we assume that they are independent,
 * (even if likely they are not).
 */
static double smart_afr(uint64_t* smart)
{
	double afr = 0;

	if (smart[5] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_5, smart[5]);

	if (smart[187] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_187, smart[187]);

	if (smart[188] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_188, smart[188]);

	if (smart[193] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_193, smart[193]);

	if (smart[197] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_197, smart[197]);

	if (smart[198] != SMART_UNASSIGNED)
		afr += smart_afr_value(AFR_198, smart[198]);

	return afr;
}

/**
 * Factorial.
 */
static double fact(unsigned n)
{
	double v = 1;

	while (n > 1)
		v *= n--;

	return v;
}

/**
 * Probability of having exactly ::n events in a Poisson
 * distribution with rate ::rate in a time unit.
 */
static double poisson_prob_n_failures(double rate, unsigned n)
{
	return pow(rate, n) * exp(-rate) / fact(n);
}

/**
 * Probability of having ::n or more events in a Poisson
 * distribution with rate ::rate in a time unit.
 */
static double poisson_prob_n_or_more_failures(double rate, unsigned n)
{
	double p_neg = 0;
	unsigned i;

	for (i = 0; i < n; ++i)
		p_neg += poisson_prob_n_failures(rate, n - 1 - i);

	return 1 - p_neg;
}

/**
 * Probability of having data loss in a RAID system with the specified ::redundancy
 * supposing the specified ::array_failure_rate, and ::replace_rate.
 */
static double raid_prob_of_one_or_more_failures(double array_failure_rate, double replace_rate, unsigned n, unsigned redundancy)
{
	unsigned i;
	double MTBF;
	double MTTR;
	double MTTDL;
	double raid_failure_rate;

	/*
	 * Use the MTTDL model (Mean Time To Data Loss) to estimate the
	 * failure rate of the array.
	 *
	 * See:
	 * Garth Alan Gibson, "Redundant Disk Arrays: Reliable, Parallel Secondary Storage", 1990
	 */

	/* get the Mean Time Between Failure of a single disk */
	/* from the array failure rate */
	MTBF = n / array_failure_rate;

	/* get the Mean Time Between Repair (the time that a failed disk is replaced) */
	/* from the repair rate */
	MTTR = 1.0 / replace_rate;

	/* use the approximated MTTDL equation */
	MTTDL = pow(MTBF, redundancy + 1) / pow(MTTR, redundancy);
	for (i = 0; i < redundancy + 1; ++i)
		MTTDL /= n - i;

	/* the raid failure rate is just the inverse of the MTTDL */
	raid_failure_rate = 1.0 / MTTDL;

	/* probability of at least one RAID failure */
	/* note that is almost equal at the probabilty of */
	/* the first failure. */
	return poisson_prob_n_or_more_failures(raid_failure_rate, 1);
}

/**
 * Prints a string with space padding.
 */
static void printl(const char* str, size_t pad)
{
	size_t len;

	printf(str);

	len = strlen(str);

	while (len < pad) {
		printf(" ");
		++len;
	}
}

/**
 * Prints a probability with space padding.
 */
static void printp(double v, size_t pad)
{
	char buf[64];
	const char* s = " %%";

	if (v > 0.1)
		snprintf(buf, sizeof(buf), "%5.2f%s", v, s);
	else if (v > 0.01)
		snprintf(buf, sizeof(buf), "%6.3f%s", v, s);
	else if (v > 0.001)
		snprintf(buf, sizeof(buf), "%7.4f%s", v, s);
	else if (v > 0.0001)
		snprintf(buf, sizeof(buf), "%8.5f%s", v, s);
	else if (v > 0.00001)
		snprintf(buf, sizeof(buf), "%9.6f%s", v, s);
	else if (v > 0.000001)
		snprintf(buf, sizeof(buf), "%10.7f%s", v, s);
	else if (v > 0.0000001)
		snprintf(buf, sizeof(buf), "%11.8f%s", v, s);
	else if (v > 0.00000001)
		snprintf(buf, sizeof(buf), "%12.9f%s", v, s);
	else if (v > 0.000000001)
		snprintf(buf, sizeof(buf), "%13.10f%s", v, s);
	else if (v > 0.0000000001)
		snprintf(buf, sizeof(buf), "%14.11f%s", v, s);
	else if (v > 0.00000000001)
		snprintf(buf, sizeof(buf), "%15.12f%s", v, s);
	else if (v > 0.000000000001)
		snprintf(buf, sizeof(buf), "%16.13f%s", v, s);
	else
		snprintf(buf, sizeof(buf), "%17.14f%s", v, s);
	printl(buf, pad);
}

static void state_smart(unsigned n, tommy_list* low)
{
	tommy_node* i;
	unsigned j;
	size_t device_pad;
	size_t serial_pad;
	double array_failure_rate;
	double p_at_least_one_failure;

	/* compute lengths for padding */
	device_pad = 0;
	serial_pad = 0;
	for (i = tommy_list_head(low); i != 0; i = i->next) {
		size_t len;
		devinfo_t* devinfo = i->data;

		len = strlen(devinfo->file);
		if (len > device_pad)
			device_pad = len;

		len = strlen(devinfo->smart_serial);
		if (len > serial_pad)
			serial_pad = len;
	}

	printf("SnapRAID SMART report:\n");
	printf("\n");
	printf("   Temp");
	printf("  Power");
	printf("  Error");
	printf(" AFP");
	printf(" Size");
	printf("\n");
	printf("     C�");
	printf(" OnDays");
	printf("  Count");
	printf("   %%");
	printf("   TB");
	printf("  "); printl("Serial", serial_pad);
	printf("  "); printl("Device", device_pad);
	printf("  Disk");
	printf("\n");
	/*      |<##################################################################72>|####80>| */
	printf(" -----------------------------------------------------------------------\n");

	array_failure_rate = 0;
	for (i = tommy_list_head(low); i != 0; i = i->next) {
		devinfo_t* devinfo = i->data;
		double afr;

		if (devinfo->smart[194] != SMART_UNASSIGNED)
			printf("%7" PRIu64, devinfo->smart[194]);
		else if (devinfo->smart[190] != SMART_UNASSIGNED)
			printf("%7" PRIu64, devinfo->smart[190]);
		else
			printf("      -");

		if (devinfo->smart[9] != SMART_UNASSIGNED)
			printf("%7" PRIu64, devinfo->smart[9] / 24);
		else
			printf("      -");

		if (devinfo->smart[SMART_ERROR] != SMART_UNASSIGNED)
			printf("%6" PRIu64, devinfo->smart[SMART_ERROR]);
		else
			printf("     -");

		afr = smart_afr(devinfo->smart);

		/* use only afr of disks in the array */
		if (devinfo->parent != 0)
			array_failure_rate += afr;

		printf("%5.0f", poisson_prob_n_or_more_failures(afr, 1) * 100);

		if (devinfo->smart[SMART_SIZE] != SMART_UNASSIGNED)
			printf("  %2.1f", devinfo->smart[SMART_SIZE] / 1E12);
		else
			printf("    -");

		printf("  ");
		if (*devinfo->smart_serial)
			printl(devinfo->smart_serial, serial_pad);
		else
			printl("-", serial_pad);

		printf("  ");
		if (*devinfo->file)
			printl(devinfo->file, device_pad);
		else
			printl("-", device_pad);

		printf("  ");
		if (*devinfo->name)
			printf("%s", devinfo->name);
		else
			printf("- (not in stats)");

		printf("\n");
	}

	printf("\n");

	/*      |<##################################################################72>|####80>| */
	printf("The AFP (Annual Failure Probability) is the probability that the disk is\n");
	printf("going to fail in the next year.\n");
	printf("\n");

	/*
	 * The probability of one and of at least one failure is computed assuming
	 * a Poisson distribution with the estimated array failure rate.
	 */
	p_at_least_one_failure = poisson_prob_n_or_more_failures(array_failure_rate, 1);

	/*      |<##################################################################72>|####80>| */
	printf("Probability of at least one disk failure in the next year is: %.0f %%\n", p_at_least_one_failure * 100);
	printf("\n");

	/*      |<##################################################################72>|####80>| */
	printf("Probability of data loss in the next year for different parity and\n");
	printf("scrub/repair times:\n");
	printf("\n");
	printf("  Parity  1 Week                 1 Month              3 Months\n");
	printf(" -----------------------------------------------------------------------\n");
	for (j = 0; j < RAID_PARITY_MAX; ++j) {
		const char* sep = "    ";

		printf("%6u", j + 1);
		printf(sep);
		printp(raid_prob_of_one_or_more_failures(array_failure_rate, 365.0 / 7, n, j + 1) * 100, 20);
		printf(sep);
		printp(raid_prob_of_one_or_more_failures(array_failure_rate, 365.0 / 30, n, j + 1) * 100, 18);
		printf(sep);
		printp(raid_prob_of_one_or_more_failures(array_failure_rate, 365.0 / 90, n, j + 1) * 100, 14);
		printf("\n");
	}

	printf("\n");

	/*      |<##################################################################72>|####80>| */
	printf("These are the probabilities that in the next year you'll have a sequence\n");
	printf("of failures that the parity WONT be able to recover, assuming that you\n");
	printf("regularly scrub and repair the full array in the specified time.\n");
}

void state_device(struct snapraid_state* state, int operation)
{
	tommy_node* i;
	unsigned j;
	tommy_list high;
	tommy_list low;

	switch (operation) {
	case DEVICE_UP : printf("Spinup...\n"); break;
	case DEVICE_DOWN : printf("Spindown...\n"); break;
	}

	tommy_list_init(&high);
	tommy_list_init(&low);

	/* for all disks */
	for (i = state->disklist; i != 0; i = i->next) {
		struct snapraid_disk* disk = i->data;
		devinfo_t* entry;

		entry = calloc_nofail(1, sizeof(devinfo_t));

		entry->device = disk->device;
		pathcpy(entry->name, sizeof(entry->name), disk->name);
		pathcpy(entry->mount, sizeof(entry->mount), disk->dir);

		tommy_list_insert_tail(&high, &entry->node, entry);
	}

	/* for all parities */
	for (j = 0; j < state->level; ++j) {
		devinfo_t* entry;

		entry = calloc_nofail(1, sizeof(devinfo_t));

		entry->device = state->parity[j].device;
		pathcpy(entry->name, sizeof(entry->name), lev_config_name(j));
		pathcpy(entry->mount, sizeof(entry->mount), state->parity[j].path);
		pathcut(entry->mount); /* remove the parity file */

		tommy_list_insert_tail(&high, &entry->node, entry);
	}

	if (devquery(&high, &low, operation) != 0) {
		switch (operation) {
		case DEVICE_UP : fprintf(stderr, "Spinup"); break;
		case DEVICE_DOWN : fprintf(stderr, "Spindown"); break;
		case DEVICE_LIST : fprintf(stderr, "List"); break;
		case DEVICE_SMART : fprintf(stderr, "SMART"); break;
		}
		fprintf(stderr, " unsupported in this platform.\n");
	}

#ifndef _WIN32
	if (operation == DEVICE_LIST) {
		for (i = tommy_list_head(&low); i != 0; i = i->next) {
			devinfo_t* devinfo = i->data;
			devinfo_t* parent = devinfo->parent;

			printf("%u:%u\t%s\t%u:%u\t%s\t%s\n", major(devinfo->device), minor(devinfo->device), devinfo->file, major(parent->device), minor(parent->device), parent->file, parent->name);
		}
	}
#endif

	if (operation == DEVICE_SMART) {
		/* count the logical disks forming the array */
		unsigned count = state->level;
		for (i = state->disklist; i != 0; i = i->next)
			++count;
		state_smart(count, &low);
	}

	tommy_list_foreach(&high, free);
	tommy_list_foreach(&low, free);
}
