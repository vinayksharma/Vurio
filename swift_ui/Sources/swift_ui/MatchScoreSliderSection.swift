import SwiftUI

struct MatchScoreSliderSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    
    // Values representing DISTANCE (1.0 - similarity)
    @State private var duplicateDistance: Double = 0.0
    @State private var similarDistance: Double = 0.20
    
    // Max distance on the slider scale
    let maxDistance: Double = 0.20
    @State private var showPopover = false
    
    var body: some View {
        VStack(alignment: .leading, spacing: 15) {
            HStack {
                Button(action: {
                    showPopover.toggle()
                }) {
                    Image(systemName: "info.circle")
                        .foregroundColor(Theme.textSecondary)
                }
                .buttonStyle(PlainButtonStyle())
                .popover(isPresented: $showPopover, arrowEdge: .trailing) {
                    ASTPopoverContent(textContent: viewModel.configLongDescriptions["duplicates.threshold.duplicate"] ?? "[]")
                        .font(.callout)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding()
                        .frame(width: 350)
                }
                
                Text("Match Score Thresholds")
                    .frame(width: 350, alignment: .leading)
            }
            
            VurioSlider(
                duplicateValue: $duplicateDistance,
                similarValue: $similarDistance,
                range: 0.0...maxDistance,
                formatValue: { val in (1.0 - val).formatted(.number.precision(.fractionLength(3))) },
                duplicateHelpText: viewModel.configDescriptions["duplicates.threshold.duplicate"] ?? "Adjust exact duplicates threshold",
                similarHelpText: viewModel.configDescriptions["duplicates.threshold.similarity"] ?? "Adjust similar images threshold"
            )
            .frame(height: 42)
        }
        .onAppear {
            loadValues()
        }
        .onChange(of: duplicateDistance) { _, _ in saveValues() }
        .onChange(of: similarDistance) { _, _ in saveValues() }
        .onChange(of: viewModel.config["duplicates.threshold.duplicate"]) { _, _ in loadValues() }
        .onChange(of: viewModel.config["duplicates.threshold.similarity"]) { _, _ in loadValues() }
    }
    
    private func loadValues() {
        if let dupStr = viewModel.config["duplicates.threshold.duplicate"], let dupVal = Double(dupStr) {
            let dist = 1.0 - dupVal
            if abs(duplicateDistance - dist) > 0.0001 {
                duplicateDistance = min(max(0.0, dist), maxDistance)
            }
        }
        if let simStr = viewModel.config["duplicates.threshold.similarity"], let simVal = Double(simStr) {
            let dist = 1.0 - simVal
            if abs(similarDistance - dist) > 0.0001 {
                similarDistance = min(max(0.0, dist), maxDistance)
            }
        }
    }
    
    private func saveValues() {
        let dupVal = 1.0 - duplicateDistance
        let simVal = 1.0 - similarDistance
        
        let newDupStr = String(format: "%.5f", dupVal)
        let newSimStr = String(format: "%.5f", simVal)
        
        if viewModel.config["duplicates.threshold.duplicate"] != newDupStr {
            viewModel.config["duplicates.threshold.duplicate"] = newDupStr
            viewModel.modifiedProperties.insert("duplicates.threshold.duplicate")
        }
        
        if viewModel.config["duplicates.threshold.similarity"] != newSimStr {
            viewModel.config["duplicates.threshold.similarity"] = newSimStr
            viewModel.modifiedProperties.insert("duplicates.threshold.similarity")
        }
    }
}

struct VurioSlider: View {
    @Binding var duplicateValue: Double
    @Binding var similarValue: Double
    let range: ClosedRange<Double>
    var formatValue: ((Double) -> String)? = nil
    var showDuplicateThumb: Bool = true
    var showSimilarThumb: Bool = true
    var duplicateHelpText: String = "Adjust exact duplicates threshold"
    var similarHelpText: String = "Adjust similar images threshold"
    
    @State private var isHoveringSimilar = false
    @State private var isHoveringDuplicate = false
    @AppStorage("advancedOverlayEnabled") private var advancedOverlayEnabled = false
    
    var body: some View {
        GeometryReader { geometry in
            let width = geometry.size.width
            let height = geometry.size.height
            let centerY = height / 2
            let thumbSize: CGFloat = 16
            
            ZStack(alignment: .topLeading) {
                // Background Track
                RoundedRectangle(cornerRadius: 4)
                    .fill(Theme.border)
                    .frame(width: width, height: 6)
                    .offset(y: centerY - 3)
                
                // Similar Images Range Track (Muted Green)
                if showSimilarThumb {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Theme.success.opacity(0.5))
                        .frame(width: max(0, xPosition(for: similarValue, in: width)), height: 6)
                        .offset(y: centerY - 3)
                }
                
                // Duplicates Range Track (Red)
                if showDuplicateThumb {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Theme.error)
                        .frame(width: max(0, xPosition(for: duplicateValue, in: width)), height: 6)
                        .offset(y: centerY - 3)
                }
                
                // Similar Images Thumb (Pointing Down)
                if showSimilarThumb {
                    TriangleDown()
                        .fill(Color.white.opacity(0.2))
                        .background(.ultraThinMaterial, in: TriangleDown())
                        .overlay(
                            TriangleDown()
                                .stroke(LinearGradient(gradient: Gradient(colors: [.white.opacity(0.9), .white.opacity(0.2)]), startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1)
                        )
                        .shadow(color: isHoveringSimilar ? Theme.success : .black.opacity(0.3), radius: isHoveringSimilar ? 5 : 2, x: 0, y: isHoveringSimilar ? 0 : 2)
                        .frame(width: thumbSize, height: thumbSize)
                        .padding(8)
                        .contentShape(Circle())
                        .offset(x: xPosition(for: similarValue, in: width) - (thumbSize/2 + 8), y: centerY - thumbSize - 10)
                        .onHover { hovering in
                            withAnimation(.easeInOut(duration: 0.15)) {
                                isHoveringSimilar = hovering
                            }
                        }
                        .help(similarHelpText)
                        .gesture(
                            DragGesture()
                                .onChanged { value in
                                    let newValue = valueFor(x: value.location.x, in: width)
                                    similarValue = min(max(range.lowerBound, newValue), range.upperBound)
                                    if similarValue < duplicateValue {
                                        duplicateValue = similarValue
                                    }
                                }
                        )
                }
                
                // Duplicates Thumb (Pointing Up)
                if showDuplicateThumb {
                    TriangleUp()
                        .fill(Color.white.opacity(0.2))
                        .background(.ultraThinMaterial, in: TriangleUp())
                        .overlay(
                            TriangleUp()
                                .stroke(LinearGradient(gradient: Gradient(colors: [.white.opacity(0.9), .white.opacity(0.2)]), startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1)
                        )
                        .shadow(color: isHoveringDuplicate ? Theme.error : .black.opacity(0.3), radius: isHoveringDuplicate ? 5 : 2, x: 0, y: isHoveringDuplicate ? 0 : 2)
                        .frame(width: thumbSize, height: thumbSize)
                        .padding(8)
                        .contentShape(Circle())
                        .offset(x: xPosition(for: duplicateValue, in: width) - (thumbSize/2 + 8), y: centerY - 6)
                        .onHover { hovering in
                            withAnimation(.easeInOut(duration: 0.15)) {
                                isHoveringDuplicate = hovering
                            }
                        }
                        .help(duplicateHelpText)
                        .gesture(
                            DragGesture()
                                .onChanged { value in
                                    let newValue = valueFor(x: value.location.x, in: width)
                                    duplicateValue = min(max(range.lowerBound, newValue), range.upperBound)
                                    if duplicateValue > similarValue {
                                        similarValue = duplicateValue
                                    }
                                }
                        )
                }
                
                if advancedOverlayEnabled, let format = formatValue {
                    let mutedGreen = Theme.success
                    
                    if showSimilarThumb {
                        let simX = xPosition(for: similarValue, in: width)
                        let simShowRight = simX < width - 40
                        Text(format(similarValue))
                            .font(.custom("Menlo", size: 9))
                            .foregroundColor(mutedGreen)
                            .frame(width: 40, alignment: simShowRight ? .leading : .trailing)
                            .offset(x: simShowRight ? simX + thumbSize/2 + 4 : simX - thumbSize/2 - 44, y: centerY - thumbSize + 2)
                    }
                    
                    if showDuplicateThumb {
                        let dupX = xPosition(for: duplicateValue, in: width)
                        let dupShowRight = dupX < width - 40
                        Text(format(duplicateValue))
                            .font(.custom("Menlo", size: 9))
                            .foregroundColor(mutedGreen)
                            .frame(width: 40, alignment: dupShowRight ? .leading : .trailing)
                            .offset(x: dupShowRight ? dupX + thumbSize/2 + 4 : dupX - thumbSize/2 - 44, y: centerY + 4)
                    }
                }
            }
        }
    }
    
    private func xPosition(for value: Double, in width: CGFloat) -> CGFloat {
        let percentage = (value - range.lowerBound) / (range.upperBound - range.lowerBound)
        return CGFloat(percentage) * width
    }
    
    private func valueFor(x: CGFloat, in width: CGFloat) -> Double {
        let percentage = Double(x / width)
        return range.lowerBound + percentage * (range.upperBound - range.lowerBound)
    }
}

struct TriangleDown: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let radius: CGFloat = 2.0
        
        let p1 = CGPoint(x: rect.minX, y: rect.minY)
        let p2 = CGPoint(x: rect.maxX, y: rect.minY)
        let p3 = CGPoint(x: rect.midX, y: rect.maxY)
        
        path.move(to: CGPoint(x: (p1.x + p2.x) / 2, y: (p1.y + p2.y) / 2))
        path.addArc(tangent1End: p2, tangent2End: p3, radius: radius)
        path.addArc(tangent1End: p3, tangent2End: p1, radius: radius)
        path.addArc(tangent1End: p1, tangent2End: p2, radius: radius)
        path.closeSubpath()
        
        return path
    }
}

struct TriangleUp: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let radius: CGFloat = 2.0
        
        let p1 = CGPoint(x: rect.midX, y: rect.minY)
        let p2 = CGPoint(x: rect.maxX, y: rect.maxY)
        let p3 = CGPoint(x: rect.minX, y: rect.maxY)
        
        path.move(to: CGPoint(x: (p1.x + p2.x) / 2, y: (p1.y + p2.y) / 2))
        path.addArc(tangent1End: p2, tangent2End: p3, radius: radius)
        path.addArc(tangent1End: p3, tangent2End: p1, radius: radius)
        path.addArc(tangent1End: p1, tangent2End: p2, radius: radius)
        path.closeSubpath()
        
        return path
    }
}
