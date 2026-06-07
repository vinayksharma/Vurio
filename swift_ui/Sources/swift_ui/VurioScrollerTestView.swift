import SwiftUI

struct VurioScrollerTestView: View {
    // Precompute alternating heights to test variable height support
    let itemHeights: [CGFloat] = (0..<100_000).map { i in
        switch i % 4 {
        case 0: return 120.0
        case 1: return 200.0
        case 2: return 80.0
        default: return 300.0
        }
    }
    @State private var selection: Int? = 0
    
    var body: some View {
        VStack(spacing: 0) {
            Text("Vurio Scroller Test Harness")
                .font(.title2)
                .fontWeight(.bold)
                .padding()
                .frame(maxWidth: .infinity)
                .background(Color.black.opacity(0.3))
            
            GeometryReader { geo in
                VurioScroller(
                    itemHeights: itemHeights,
                    scrollbarInsets: geo.safeAreaInsets,
                    selection: $selection
                ) { index in
                    TestRowView(index: index, height: itemHeights[index])
                }
                .background(Color.black.opacity(0.1))
            }
        }
    }
}

struct TestRowView: View {
    let index: Int
    let height: CGFloat
    @State private var text: String = ""
    @State private var selectedTab = 0
    
    var body: some View {
        HStack(spacing: 20) {
            Text("Group #\(index)")
                .font(.title3)
                .fontWeight(.semibold)
                .frame(width: 120, alignment: .leading)
            
            VStack(alignment: .leading, spacing: 10) {
                // Test 1: Typing text should intercept spacebar and arrow keys naturally
                TextField("Edit Description...", text: $text)
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 300)
                
                // Test 2: Arrow keys should navigate segmented control, NOT scroll the page
                Picker("Options", selection: $selectedTab) {
                    Text("Details").tag(0)
                    Text("Metadata").tag(1)
                    Text("Actions").tag(2)
                }
                .pickerStyle(SegmentedPickerStyle())
                .frame(width: 300)
            }
            
            Spacer()
            
            Image(systemName: "photo.on.rectangle")
                .font(.system(size: 40))
                .foregroundColor(.gray)
        }
        .padding(.horizontal, 24)
        .frame(height: height)
        .background(index % 2 == 0 ? Color.white.opacity(0.02) : Color.white.opacity(0.05))
        // Explicitly clip to prevent weird layout overdraw
        .clipped()
    }
}
