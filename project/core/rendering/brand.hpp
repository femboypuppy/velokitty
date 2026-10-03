#pragma once

// aimwhere branding: the name and both logos, in one place so the menu, the intro and the watermark can't
// drift apart. The logos are plain SVG for xdraw::load_svg (nanosvg): strokes and circles only, no text --
// the wordmark is drawn letter by letter as geometric monoline paths.
namespace rendering::brand {

	constexpr auto name{ "aimwhere" };

	/// Reticle with a question mark where the dot should be. 24x24, drawn white so the draw call's tint colour
	/// is the colour it shows in.
	constexpr auto icon_svg{ R"(<svg xmlns="http://www.w3.org/2000/svg" width="24.00" height="24.00" viewBox="0 0 24.00 24.00"><circle cx="12.00" cy="12.00" r="8.00" fill="none" stroke="#FFFFFF" stroke-width="2.20"/><path d="M12.00 0.80 L12.00 5.00 M12.00 19.00 L12.00 23.20 M0.80 12.00 L5.00 12.00 M19.00 12.00 L23.20 12.00" fill="none" stroke="#FFFFFF" stroke-width="2.20" stroke-linecap="round"/><path d="M9.70 9.90 C9.70 7.60 14.30 7.60 14.30 9.90 C14.30 11.60 12.00 11.50 12.00 13.40" fill="none" stroke="#FFFFFF" stroke-width="1.70" stroke-linecap="round" stroke-linejoin="round"/><circle cx="12.00" cy="16.20" r="1.05" fill="#FFFFFF"/></svg>)" };
	constexpr float icon_view{ 24.0f };

	/// Reticle in the accent colour, then "aim" in white and "where" in the accent. Shown in its own colours.
	constexpr auto splash_svg{ R"(<svg xmlns="http://www.w3.org/2000/svg" width="137.80" height="25.00" viewBox="0 0 137.80 25.00"><circle cx="12.40" cy="12.40" r="7.60" fill="none" stroke="#ADC0FF" stroke-width="2.09"/><path d="M12.40 1.76 L12.40 5.75 M12.40 19.05 L12.40 23.04 M1.76 12.40 L5.75 12.40 M19.05 12.40 L23.04 12.40" fill="none" stroke="#ADC0FF" stroke-width="2.09" stroke-linecap="round"/><path d="M10.21 10.40 C10.21 8.22 14.59 8.22 14.59 10.40 C14.59 12.02 12.40 11.92 12.40 13.73" fill="none" stroke="#ADC0FF" stroke-width="1.61" stroke-linecap="round" stroke-linejoin="round"/><circle cx="12.40" cy="16.39" r="1.00" fill="#ADC0FF"/><path d="M39.8 15 A5 5 0 1 1 39.8 14.99 M39.8 10 V20 M44.0 10 V20 M48.2 20 V10 M48.2 14 A4 4 0 0 1 56.2 14 V20 M56.2 14 A4 4 0 0 1 64.2 14 V20" fill="none" stroke="#FFFFFF" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/><circle cx="44.0" cy="5.6" r="1.61" fill="#FFFFFF"/><path d="M68.4 10 L72.0 20 L75.60000000000001 12.4 L79.2 20 L82.80000000000001 10 M87.0 4 V20 M87.0 15 A5 5 0 0 1 97.0 15 V20 M101.2 15 H111.2 A5 5 0 1 0 109.74000000000001 18.54 M115.4 10 V20 M115.4 15 A5 5 0 0 1 121.60000000000001 10.2 M125.80000000000001 15 H135.8 A5 5 0 1 0 134.34 18.54" fill="none" stroke="#ADC0FF" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)" };
	constexpr float splash_view_w{ 137.80f };

} // namespace rendering::brand
