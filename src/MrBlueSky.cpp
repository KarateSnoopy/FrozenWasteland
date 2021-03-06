#include "FrozenWasteland.hpp"
#include "dsp/decimator.hpp"
#include "dsp/digital.hpp"
#include "filters/biquad.h"

using namespace std;

#define BANDS 16

struct MrBlueSky : Module {
	enum ParamIds {
		BG_PARAM,
		ATTACK_PARAM = BG_PARAM + BANDS,
		DECAY_PARAM,
		CARRIER_Q_PARAM,
		MOD_Q_PARAM,
		BAND_OFFSET_PARAM,
		GMOD_PARAM,
		GCARR_PARAM, 	
		G_PARAM,
		SHAPE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		CARRIER_IN,		
		IN_MOD = CARRIER_IN + BANDS,
		IN_CARR,
		ATTACK_INPUT,
		DECAY_INPUT,
		CARRIER_Q_INPUT,
		MOD_Q_INPUT,
		SHIFT_BAND_OFFSET_LEFT_INPUT,
		SHIFT_BAND_OFFSET_RIGHT_INPUT,
		SHIFT_BAND_OFFSET_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		MOD_OUT,
		OUT = MOD_OUT + BANDS,
		NUM_OUTPUTS
	};
	enum LightIds {
		LEARN_LIGHT,
		NUM_LIGHTS
	};
	Biquad* iFilter[2*BANDS];
	Biquad* cFilter[2*BANDS];
	float mem[BANDS] = {0};
	float freq[BANDS] = {125,185,270,350,430,530,630,780,950,1150,1380,1680,2070,2780,3800,6400};
	float peaks[BANDS] = {0};
	float lastCarrierQ = 0;
	float lastModQ = 0;
	int bandOffset = 0;
	SchmittTrigger shiftLeftTrigger;
	SchmittTrigger shiftRightTrigger;

	MrBlueSky() : Module(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS) {
		for(int i=0; i<2*BANDS; i++) {
			iFilter[i] = new Biquad(bq_type_bandpass, freq[i%BANDS] / engineGetSampleRate(), 5, 6);
			cFilter[i] = new Biquad(bq_type_bandpass, freq[i%BANDS] / engineGetSampleRate(), 5, 6);
		};
		shiftLeftTrigger.setThresholds(0.0, 0.01);
		shiftRightTrigger.setThresholds(0.0, 0.01);
	}

	void step() override;

	void reset() override {
		bandOffset =0;
	}

};

void MrBlueSky::step() {
	// Band Offset Processing
	bandOffset = params[BAND_OFFSET_PARAM].value;
	if(inputs[SHIFT_BAND_OFFSET_INPUT].active) {
		bandOffset += inputs[SHIFT_BAND_OFFSET_INPUT].value;
		//Hack until I can do int clamping
		if(bandOffset <= -BANDS) {
			bandOffset = BANDS -1;
		}
		if(bandOffset >= BANDS) {
			bandOffset = -BANDS + 1;
		}
	}

	/* Disabling For now :(
	if(inputs[SHIFT_BAND_OFFSET_LEFT_INPUT].active) { 
		if (shiftLeftTrigger.process(inputs[SHIFT_BAND_OFFSET_LEFT_INPUT].value)) {
			bandOffset = (bandOffset - 1);
			if(bandOffset <= -BANDS) {
				bandOffset = BANDS -1;
			}
		}
	}

	if(inputs[SHIFT_BAND_OFFSET_RIGHT_INPUT].active) {
		if (shiftRightTrigger.process(inputs[SHIFT_BAND_OFFSET_RIGHT_INPUT].value)) {
			bandOffset = (bandOffset + 1);
			if(bandOffset >= BANDS) {
				bandOffset = -BANDS + 1;
			}
		}
	}
	*/

	//So some vocoding!
	float inM = inputs[IN_MOD].value/5;
	float inC = inputs[IN_CARR].value/5;
	const float slewMin = 0.001;
	const float slewMax = 500.0;
	const float shapeScale = 1/10.0;
	const float qEpsilon = 0.1;
	float attack = params[ATTACK_PARAM].value;
	float decay = params[DECAY_PARAM].value;
	if(inputs[ATTACK_INPUT].active) {
		attack += clampf(inputs[ATTACK_INPUT].value / 20.0,-0.25,.25);
	}
	if(inputs[DECAY_INPUT].active) {
		decay += clampf(inputs[DECAY_INPUT].value / 20.0,-0.25,.25);
	}
	float slewAttack = slewMax * powf(slewMin / slewMax, attack);
	float slewDecay = slewMax * powf(slewMin / slewMax, decay);
	float out = 0.0;

	//Check Mod Q
	float currentQ = params[MOD_Q_PARAM].value;
	if(inputs[MOD_Q_PARAM].active) {
		currentQ += inputs[MOD_Q_INPUT].value;
	}

	currentQ = clampf(currentQ,1.0,15.0);
	if (abs(currentQ - lastModQ) >= qEpsilon ) {
		for(int i=0; i<2*BANDS; i++) {
			iFilter[i]->setQ(currentQ);
			}
		lastModQ = currentQ;
	}

	//Check Carrier Q
	currentQ = params[CARRIER_Q_PARAM].value;
	if(inputs[CARRIER_Q_INPUT].active) {
		currentQ += inputs[CARRIER_Q_INPUT].value;
	}

	currentQ = clampf(currentQ,1.0,15.0);
	if (abs(currentQ - lastCarrierQ) >= qEpsilon ) {
		for(int i=0; i<2*BANDS; i++) {
			cFilter[i]->setQ(currentQ);
			}
		lastCarrierQ = currentQ;
	}



	//First process all the modifier bands
	for(int i=0; i<BANDS; i++) {
		float coeff = mem[i];
		float peak = abs(iFilter[i+BANDS]->process(iFilter[i]->process(inM*params[GMOD_PARAM].value)));
		if (peak>coeff) {
			coeff += slewAttack * shapeScale * (peak - coeff) / engineGetSampleRate();
			if (coeff > peak)
				coeff = peak;
		}
		else if (peak < coeff) {
			coeff -= slewDecay * shapeScale * (coeff - peak) / engineGetSampleRate();
			if (coeff < peak)
				coeff = peak;
		}
		peaks[i]=peak;
		mem[i]=coeff;
		outputs[MOD_OUT+i].value = coeff * 5.0;
	}

	//Then process carrier bands. Mod bands are normalled to their matched carrier band unless an insert
	for(int i=0; i<BANDS; i++) {
		float coeff;
		if(inputs[(CARRIER_IN+i+bandOffset) % BANDS].active) {
			coeff = inputs[CARRIER_IN+i+bandOffset].value / 5.0;
		} else {
			coeff = mem[(i+bandOffset) % BANDS];
		}
		
		float bandOut = cFilter[i+BANDS]->process(cFilter[i]->process(inC*params[GCARR_PARAM].value)) * coeff * params[BG_PARAM+i].value;
		out += bandOut;
	}
	outputs[OUT].value = out * 5 * params[G_PARAM].value;

}

struct MrBlueSkyBandDisplay : TransparentWidget {
	MrBlueSky *module;
	std::shared_ptr<Font> font;

	MrBlueSkyBandDisplay() {
		font = Font::load(assetPlugin(plugin, "res/fonts/Sudo.ttf"));
	}

	void draw(NVGcontext *vg) override {
		nvgFontSize(vg, 14);
		nvgFontFaceId(vg, font->handle);
		nvgStrokeWidth(vg, 2);
		nvgTextLetterSpacing(vg, -2);
		nvgTextAlign(vg, NVG_ALIGN_CENTER);
		//static const int portX0[4] = {20, 63, 106, 149};
		for (int i=0; i<BANDS; i++) {
			char fVal[10];
			snprintf(fVal, sizeof(fVal), "%1i", (int)module->freq[i]);
			nvgFillColor(vg,nvgRGBA(rescalef(clampf(module->peaks[i],0,1),0,1,0,255), 0, 0, 255));
			nvgText(vg, 56 + 43*i, 30, fVal, NULL);
		}
	}
};

struct BandOffsetDisplay : TransparentWidget {
	MrBlueSky *module;
	int frame = 0;
	std::shared_ptr<Font> font;

	BandOffsetDisplay() {
		font = Font::load(assetPlugin(plugin, "res/fonts/01 Digit.ttf"));
	}

	void drawDuration(NVGcontext *vg, Vec pos, float bandOffset) {
		nvgFontSize(vg, 20);
		nvgFontFaceId(vg, font->handle);
		nvgTextLetterSpacing(vg, -2);

		nvgFillColor(vg, nvgRGBA(0x00, 0xff, 0x00, 0xff));
		char text[128];
		snprintf(text, sizeof(text), " % 2.0f", bandOffset);
		nvgText(vg, pos.x + 22, pos.y, text, NULL);
	}

	void draw(NVGcontext *vg) override {
	
		drawDuration(vg, Vec(0, box.size.y - 150), module->bandOffset);
	}
};

MrBlueSkyWidget::MrBlueSkyWidget() {
	MrBlueSky *module = new MrBlueSky();
	setModule(module);
	box.size = Vec(15*52, 380);

	{
		SVGPanel *panel = new SVGPanel();
		panel->box.size = box.size;
		panel->setBackground(SVG::load(assetPlugin(plugin, "res/MrBlueSky.svg")));
		addChild(panel);
	}

	MrBlueSkyBandDisplay *bandDisplay = new MrBlueSkyBandDisplay();
	bandDisplay->module = module;
	bandDisplay->box.pos = Vec(12, 12);
	bandDisplay->box.size = Vec(700, 70);
	addChild(bandDisplay);

	{
		BandOffsetDisplay *offsetDisplay = new BandOffsetDisplay();
		offsetDisplay->module = module;
		offsetDisplay->box.pos = Vec(435, 200);
		offsetDisplay->box.size = Vec(box.size.x, 150);
		addChild(offsetDisplay);
	}

	for (int i = 0; i < BANDS; i++) {
		addParam( createParam<RoundBlackKnob>(Vec(50 + 43*i, 120), module, MrBlueSky::BG_PARAM + i, 0, 2, 1));
	}
	addParam(createParam<RoundSmallBlackKnob>(Vec(34, 177), module, MrBlueSky::ATTACK_PARAM, 0.0, 0.25, 0.0));
	addParam(createParam<RoundSmallBlackKnob>(Vec(116, 177), module, MrBlueSky::DECAY_PARAM, 0.0, 0.25, 0.0));
	addParam(createParam<RoundSmallBlackKnob>(Vec(198, 177), module, MrBlueSky::CARRIER_Q_PARAM, 1.0, 15.0, 5.0));
	addParam(createParam<RoundSmallBlackKnob>(Vec(280, 177), module, MrBlueSky::MOD_Q_PARAM, 1.0, 15.0, 5.0));
	addParam(createParam<RoundSmallBlackKnob>(Vec(392, 177), module, MrBlueSky::BAND_OFFSET_PARAM, -15.5, 15.5, 0.0));
	addParam(createParam<RoundBlackKnob>(Vec(15, 255), module, MrBlueSky::GMOD_PARAM, 1, 10, 1));
	addParam(createParam<RoundBlackKnob>(Vec(70, 255), module, MrBlueSky::GCARR_PARAM, 1, 10, 1));
	addParam(createParam<RoundBlackKnob>(Vec(125, 255), module, MrBlueSky::G_PARAM, 1, 10, 1));


	for (int i = 0; i < BANDS; i++) {
		addInput(createInput<PJ301MPort>(Vec(56 + 43*i, 85), module, MrBlueSky::CARRIER_IN + i));
	}
	addInput(createInput<PJ301MPort>(Vec(10, 330), module, MrBlueSky::IN_MOD));
	addInput(createInput<PJ301MPort>(Vec(48, 330), module, MrBlueSky::IN_CARR));
	addInput(createInput<PJ301MPort>(Vec(35, 207), module, MrBlueSky::ATTACK_INPUT));
	addInput(createInput<PJ301MPort>(Vec(117, 207), module, MrBlueSky::DECAY_INPUT));
	addInput(createInput<PJ301MPort>(Vec(200, 207), module, MrBlueSky::CARRIER_Q_INPUT));
	addInput(createInput<PJ301MPort>(Vec(282, 207), module, MrBlueSky::MOD_Q_INPUT));
	//addInput(createInput<PJ301MPort>(Vec(362, 182), module, MrBlueSky::SHIFT_BAND_OFFSET_LEFT_INPUT));
	//addInput(createInput<PJ301MPort>(Vec(425, 182), module, MrBlueSky::SHIFT_BAND_OFFSET_RIGHT_INPUT));
	addInput(createInput<PJ301MPort>(Vec(394, 207), module, MrBlueSky::SHIFT_BAND_OFFSET_INPUT));

	for (int i = 0; i < BANDS; i++) {
		addOutput(createOutput<PJ301MPort>(Vec(56 + 43*i, 45), module, MrBlueSky::MOD_OUT + i));
	}
	addOutput(createOutput<PJ301MPort>(Vec(85, 330), module, MrBlueSky::OUT));

	addChild(createScrew<ScrewSilver>(Vec(15, 0)));
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 0)));
	addChild(createScrew<ScrewSilver>(Vec(15, 365)));
	addChild(createScrew<ScrewSilver>(Vec(box.size.x-30, 365)));
}
