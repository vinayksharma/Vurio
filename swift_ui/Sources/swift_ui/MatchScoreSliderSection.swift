import SwiftUI

struct MatchScoreSliderSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    
    // Values representing DISTANCE (1.0 - similarity)
    @State private var duplicateDistance: Double = 0.02
    @State private var similarDistance: Double = 0.10
    
    // Max distance on the slider scale
    let maxDistance: Double = 0.20
    
    var body: some View {
        VStack(alignment: .leading, spacing: 15) {
            HStack {
                Text("Match Score Thresholds")
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundColor(.primary)
                
                Button(action: {
                    // Placeholder for popover if needed
                }) {
                    Image(systemName: "info.circle")
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
                .help("Adjust the strictness for grouping images and identifying exact duplicates. A lower score requires a closer match.")
            }
            
            DualSlider(
                duplicateValue: $duplicateDistance,
                similarValue: $similarDistance,
                range: 0.0...maxDistance
            )
            .frame(height: 42)
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
        .onAppear {
            loadValues()
        }
        .onChange(of: duplicateDistance) { _ in saveValues() }
        .onChange(of: similarDistance) { _ in saveValues() }
        .onChange(of: viewModel.config["duplicates.threshold.duplicate"]) { _ in loadValues() }
        .onChange(of: viewModel.config["duplicates.threshold.similarity"]) { _ in loadValues() }
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

struct DualSlider: View {
    @Binding var duplicateValue: Double
    @Binding var similarValue: Double
    let range: ClosedRange<Double>
    
    @State private var isHoveringSimilar = false
    @State private var isHoveringDuplicate = false
    
    var body: some View {
        GeometryReader { geometry in
            let width = geometry.size.width
            let height = geometry.size.height
            let centerY = height / 2
            let thumbSize: CGFloat = 16
            
            ZStack(alignment: .topLeading) {
                // Background Track
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color(NSColor.separatorColor))
                    .frame(width: width, height: 6)
                    .offset(y: centerY - 3)
                
                // Similar Images Range Track (Muted Green)
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.green.opacity(0.5))
                    .frame(width: max(0, xPosition(for: similarValue, in: width)), height: 6)
                    .offset(y: centerY - 3)
                
                // Duplicates Range Track (Red)
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.red)
                    .frame(width: max(0, xPosition(for: duplicateValue, in: width)), height: 6)
                    .offset(y: centerY - 3)
                
                // Similar Images Thumb (Pointing Down)
                TriangleDown()
                    .fill(Color.white.opacity(0.2))
                    .background(.ultraThinMaterial, in: TriangleDown())
                    .overlay(
                        TriangleDown()
                            .stroke(LinearGradient(gradient: Gradient(colors: [.white.opacity(0.9), .white.opacity(0.2)]), startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1)
                    )
                    .shadow(color: isHoveringSimilar ? .green : .black.opacity(0.3), radius: isHoveringSimilar ? 5 : 2, x: 0, y: isHoveringSimilar ? 0 : 2)
                    .frame(width: thumbSize, height: thumbSize)
                    .offset(x: xPosition(for: similarValue, in: width) - thumbSize/2, y: centerY - thumbSize - 2)
                    .onHover { hovering in
                        withAnimation(.easeInOut(duration: 0.15)) {
                            isHoveringSimilar = hovering
                        }
                    }
                    .help("Adjust the Similar Images threshold. Determines the maximum distance to consider images as part of a group.")
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
                
                // Duplicates Thumb (Pointing Up)
                TriangleUp()
                    .fill(Color.white.opacity(0.2))
                    .background(.ultraThinMaterial, in: TriangleUp())
                    .overlay(
                        TriangleUp()
                            .stroke(LinearGradient(gradient: Gradient(colors: [.white.opacity(0.9), .white.opacity(0.2)]), startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1)
                    )
                    .shadow(color: isHoveringDuplicate ? .red : .black.opacity(0.3), radius: isHoveringDuplicate ? 5 : 2, x: 0, y: isHoveringDuplicate ? 0 : 2)
                    .frame(width: thumbSize, height: thumbSize)
                    .offset(x: xPosition(for: duplicateValue, in: width) - thumbSize/2, y: centerY + 2)
                    .onHover { hovering in
                        withAnimation(.easeInOut(duration: 0.15)) {
                            isHoveringDuplicate = hovering
                        }
                    }
                    .help("Adjust the Exact Duplicates threshold. Determines the maximum distance to consider images as identical.")
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
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.midX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

struct TriangleUp: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}
